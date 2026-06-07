// GPU-vs-oracle correctness gate for the ODF deform kernel.
//
// Loads the real demo fODF volume, builds the icosphere + SH basis matrix with the
// verified CPU foundation, computes the per-(voxel,direction) glyph radii on BOTH
// the CPU (double-precision oracle) and the GPU (Metal compute via Qt RHI), reads
// the GPU result back, and reports the max abs/rel difference. This proves the GPU
// SH reconstruction reproduces the CPU foundation before we invest in rendering.
//
// Headless (offscreen), no window. Standalone target — see CMakeLists.txt.

#include "odf_volume.hpp"
#include "sh_basis.hpp"
#include "icosphere.hpp"
#include "odf_types.hpp"

#include <rhi/qrhi.h>
#include <QGuiApplication>
#include <QFile>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tracto::odf;

static QShader loadShader(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const std::string niiPath =
        (argc > 1) ? argv[1] : "/Users/haoziqi/Documents/Finch-Viewer/dmri-explorer/data/odf.nii.gz";
    const QString qsbPath =
        (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("odf_deform.comp.qsb");

    // ---------------- CPU: load + build (the verified foundation) ----------------
    OdfVolume vol = LoadOdfNifti(niiPath);
    const int nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2], nC = vol.nCoeffs;
    std::printf("volume   : %dx%dx%d  nCoeffs=%d  voxels=%zu  range=[%.3e,%.3e]\n",
                nx, ny, nz, nC, vol.voxelCount(), double(vol.valueMin), double(vol.valueMax));

    const Icosphere ico = MakeIcosphere(2);  // 162 sample directions
    const int nDir = int(ico.vertexCount());
    const ShOrder order = InferShOrder(std::size_t(nC));
    const ShBasisMatrix B = BuildShBasisMatrix(ico.vertices, order);
    std::printf("icosphere: dirs=%d   basis=%zux%zu   lMax=%d kind=%s\n",
                nDir, B.nDir, B.nCoeffs, order.lMax,
                order.kind == ShBasisKind::Symmetric ? "symmetric" : "full");

    std::vector<VoxelIndex> vox;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) vox.push_back({i, j, k});
    const int nVox = int(vox.size());

    // repack selected voxels' coeffs voxel-major (the volume stores them coeff-major)
    std::vector<float> coeffsPacked(std::size_t(nVox) * nC);
    for (int v = 0; v < nVox; ++v) {
        const VoxelIndex& V = vox[v];
        for (int c = 0; c < nC; ++c)
            coeffsPacked[std::size_t(v) * nC + c] = vol.coeffs[vol.coeffIndex(V.i, V.j, V.k, c)];
    }

    // CPU oracle radii (double accumulation), radii[v*nDir + d]
    std::vector<float> radiiCpu(std::size_t(nVox) * nDir);
    for (int v = 0; v < nVox; ++v) {
        const float* cc = &coeffsPacked[std::size_t(v) * nC];
        for (int d = 0; d < nDir; ++d) {
            const float* bc = B.row(std::size_t(d));
            double s = 0.0;
            for (int c = 0; c < nC; ++c) s += double(cc[c]) * double(bc[c]);
            radiiCpu[std::size_t(v) * nDir + d] = float(s);
        }
    }

    // ---------------- GPU: Metal compute ----------------
    QRhiMetalInitParams params;
    QRhi* rhi = QRhi::create(QRhi::Metal, &params);
    if (!rhi) { std::fprintf(stderr, "FAIL: QRhi::create(Metal)\n"); return 2; }
    std::printf("gpu      : %s (%s)\n", rhi->driverInfo().deviceName.constData(), rhi->backendName());

    QShader cs = loadShader(qsbPath);
    if (!cs.isValid()) { std::fprintf(stderr, "FAIL: load shader %s\n", qPrintable(qsbPath)); return 3; }

    auto mkBuf = [&](QRhiBuffer::Type t, QRhiBuffer::UsageFlags u, quint32 sz) {
        QRhiBuffer* b = rhi->newBuffer(t, u, sz); b->create(); return b;
    };
    QRhiBuffer* bCoeff = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(coeffsPacked.size() * sizeof(float)));
    QRhiBuffer* bBasis = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(B.values.size() * sizeof(float)));
    QRhiBuffer* bRadii = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, quint32(radiiCpu.size() * sizeof(float)));
    QRhiBuffer* bUbo   = mkBuf(QRhiBuffer::Dynamic,   QRhiBuffer::UniformBuffer, 256);

    using SRB = QRhiShaderResourceBinding;
    QRhiShaderResourceBindings* srb = rhi->newShaderResourceBindings();
    srb->setBindings({
        SRB::bufferLoad (0, SRB::ComputeStage, bCoeff),
        SRB::bufferLoad (1, SRB::ComputeStage, bBasis),
        SRB::bufferStore(2, SRB::ComputeStage, bRadii),
        SRB::uniformBuffer(3, SRB::ComputeStage, bUbo),
    });
    srb->create();

    QRhiComputePipeline* pipe = rhi->newComputePipeline();
    pipe->setShaderStage({ QRhiShaderStage::Compute, cs });
    pipe->setShaderResourceBindings(srb);
    if (!pipe->create()) { std::fprintf(stderr, "FAIL: compute pipeline create\n"); return 4; }

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) { std::fprintf(stderr, "FAIL: beginOffscreenFrame\n"); return 5; }

    QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(bCoeff, coeffsPacked.data());
    u->uploadStaticBuffer(bBasis, B.values.data());
    struct UboData { int nVox, nDir, nCoeffs, _pad; } ubo{ nVox, nDir, nC, 0 };
    u->updateDynamicBuffer(bUbo, 0, sizeof(ubo), &ubo);

    cb->beginComputePass(u);
    cb->setComputePipeline(pipe);
    cb->setShaderResources(srb);
    const int total = nVox * nDir;
    cb->dispatch((total + 63) / 64, 1, 1);
    cb->endComputePass();

    QRhiReadbackResult rb;
    QRhiResourceUpdateBatch* u2 = rhi->nextResourceUpdateBatch();
    u2->readBackBuffer(bRadii, 0, quint32(radiiCpu.size() * sizeof(float)), &rb);
    cb->resourceUpdate(u2);
    rhi->endOffscreenFrame();

    if (rb.data.size() != int(radiiCpu.size() * sizeof(float))) { std::fprintf(stderr, "FAIL: readback size\n"); return 6; }
    const float* g = reinterpret_cast<const float*>(rb.data.constData());

    double maxAbs = 0.0, maxRel = 0.0;
    int worst = -1;
    for (std::size_t i = 0; i < radiiCpu.size(); ++i) {
        const double a = std::fabs(double(g[i]) - double(radiiCpu[i]));
        if (a > maxAbs) { maxAbs = a; worst = int(i); }
        const double den = std::fabs(double(radiiCpu[i]));
        if (den > 1e-9) maxRel = std::max(maxRel, a / den);
    }
    std::printf("compared : %zu radii (nVox=%d x nDir=%d)\n", radiiCpu.size(), nVox, nDir);
    std::printf("max |GPU-CPU| = %.3e    max rel = %.3e\n", maxAbs, maxRel);
    if (worst >= 0)
        std::printf("worst @ %d: gpu=%.6e  cpu=%.6e\n", worst, double(g[worst]), double(radiiCpu[worst]));

    // coeffs ~1e-3 -> radii ~1e-3; float-vs-double rounding gives ~1e-9 abs. 1e-6 is safe.
    const bool ok = (maxAbs < 1e-6);
    std::printf("\n%s\n", ok ? "DEFORM CHECK PASSED: GPU SH deform matches the CPU oracle."
                             : "DEFORM CHECK FAILED.");

    delete pipe; delete srb; delete bUbo; delete bRadii; delete bBasis; delete bCoeff; delete rhi;
    return ok ? 0 : 7;
}
