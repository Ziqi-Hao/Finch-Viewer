// Fully-GPU ODF render: SH coeffs -> Metal compute deform (positions+normals in SSBOs)
// -> instanced programmable-vertex-pulling draw, all in one offscreen frame. No CPU
// glyph mesh is uploaded. Two verifications:
//   1) read the GPU positions back and compare to the CPU oracle BuildGlyphs (numeric);
//   2) render to PNG (visual, should match odf_render.png).

#include "odf_volume.hpp"
#include "sh_basis.hpp"
#include "icosphere.hpp"
#include "glyph_builder.hpp"
#include "odf_types.hpp"

#include <rhi/qrhi.h>
#include <QGuiApplication>
#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <QVector3D>
#include <QColor>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tracto::odf;

static QShader loadShader(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}
static QVector3D qv(const Vec3& v) { return QVector3D(v.x, v.y, v.z); }

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const std::string niiPath =
        (argc > 1) ? argv[1] : "/Users/haoziqi/Documents/Finch-Viewer/dmri-explorer/data/odf.nii.gz";
    const QString shaderDir = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("/tmp/odf_rhi_build");
    const QString outPng    = (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("/tmp/odf_render_gpu.png");
    const int W = 900, H = 700;

    // ---------------- CPU foundation: load + oracle mesh (for comparison) ----------------
    OdfVolume vol = LoadOdfNifti(niiPath);
    const ShOrder order = InferShOrder(std::size_t(vol.nCoeffs));
    const Icosphere ico = MakeIcosphere(3);
    const ShBasisMatrix B = BuildShBasisMatrix(ico.vertices, order);
    const int nDir = int(ico.vertexCount());
    const int nC = vol.nCoeffs;
    const int nTri = int(ico.indices.size() / 3);

    auto colLen = [&](int c) {
        return std::sqrt(vol.affine.at(0, c) * vol.affine.at(0, c) +
                         vol.affine.at(1, c) * vol.affine.at(1, c) +
                         vol.affine.at(2, c) * vol.affine.at(2, c));
    };
    const float voxSize = std::min({colLen(0), colLen(1), colLen(2)});

    std::vector<VoxelIndex> vox;
    for (int k = 0; k < vol.dims[2]; ++k)
        for (int j = 0; j < vol.dims[1]; ++j)
            for (int i = 0; i < vol.dims[0]; ++i) vox.push_back({i, j, k});
    const int nVox = int(vox.size());

    GlyphParams gp;
    gp.scale = 0.45f * voxSize;
    gp.normalizePerGlyph = true;
    gp.clampNegative = true;
    const GlyphMesh oracle = BuildGlyphs(vol, ico, B, vox, gp);

    // ---------------- pack GPU inputs ----------------
    std::vector<float> coeffsPacked(std::size_t(nVox) * nC);
    for (int g = 0; g < nVox; ++g)
        for (int c = 0; c < nC; ++c)
            coeffsPacked[std::size_t(g) * nC + c] = vol.coeffs[vol.coeffIndex(vox[g].i, vox[g].j, vox[g].k, c)];

    std::vector<float> dirs4(std::size_t(nDir) * 4, 0.0f);
    for (int v = 0; v < nDir; ++v) {
        dirs4[std::size_t(v) * 4 + 0] = ico.vertices[v].x;
        dirs4[std::size_t(v) * 4 + 1] = ico.vertices[v].y;
        dirs4[std::size_t(v) * 4 + 2] = ico.vertices[v].z;
    }
    std::vector<float> centers4(std::size_t(nVox) * 4, 0.0f);
    for (int g = 0; g < nVox; ++g) {
        const Vec3 w = VoxelToWorld(vol.affine, vox[g]);
        centers4[std::size_t(g) * 4 + 0] = w.x;
        centers4[std::size_t(g) * 4 + 1] = w.y;
        centers4[std::size_t(g) * 4 + 2] = w.z;
    }
    const std::vector<unsigned>& idx = ico.indices;
    const quint32 posBytes = quint32(std::size_t(nVox) * nDir * 4 * sizeof(float));  // vec4 per vertex

    std::printf("volume %dx%dx%d nCoeffs=%d voxSize=%.3f  nVox=%d nDir=%d nTri=%d\n",
                vol.dims[0], vol.dims[1], vol.dims[2], nC, voxSize, nVox, nDir, nTri);

    // camera
    const QVector3D center = qv(vol.worldBounds.center());
    const Vec3 ext = vol.worldBounds.extent();
    const float diag = std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z) + 2.0f * gp.scale;
    const QVector3D eye = center + QVector3D(0.9f, -1.0f, 0.6f).normalized() * (1.6f * diag + 3.0f * gp.scale);

    // ---------------- RHI Metal ----------------
    QRhiMetalInitParams params;
    QRhi* rhi = QRhi::create(QRhi::Metal, &params);
    if (!rhi) { std::fprintf(stderr, "FAIL: QRhi::create(Metal)\n"); return 2; }
    std::printf("gpu %s (%s)\n", rhi->driverInfo().deviceName.constData(), rhi->backendName());

    QShader cs = loadShader(shaderDir + "/odf_deform_full.comp.qsb");
    QShader vs = loadShader(shaderDir + "/glyph_pull.vert.qsb");
    QShader fs = loadShader(shaderDir + "/glyph.frag.qsb");
    if (!cs.isValid() || !vs.isValid() || !fs.isValid()) { std::fprintf(stderr, "FAIL: load shaders from %s\n", qPrintable(shaderDir)); return 3; }

    auto mkBuf = [&](QRhiBuffer::Type t, QRhiBuffer::UsageFlags u, quint32 sz) {
        QRhiBuffer* b = rhi->newBuffer(t, u, sz); b->create(); return b;
    };
    QRhiBuffer* bCoeff   = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(coeffsPacked.size() * sizeof(float)));
    QRhiBuffer* bBasis   = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(B.values.size() * sizeof(float)));
    QRhiBuffer* bDirs    = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(dirs4.size() * sizeof(float)));
    QRhiBuffer* bIdxStor = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(idx.size() * sizeof(unsigned)));
    QRhiBuffer* bCenters = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(centers4.size() * sizeof(float)));
    QRhiBuffer* bRadii   = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, quint32(std::size_t(nVox) * nDir * sizeof(float)));
    QRhiBuffer* bPos     = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, posBytes);
    QRhiBuffer* bNor     = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, posBytes);
    QRhiBuffer* bCUbo    = mkBuf(QRhiBuffer::Dynamic,   QRhiBuffer::UniformBuffer, 256);
    QRhiBuffer* bRUbo    = mkBuf(QRhiBuffer::Dynamic,   QRhiBuffer::UniformBuffer, 256);
    QRhiBuffer* bIdxDraw = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,   quint32(idx.size() * sizeof(unsigned)));

    using SRB = QRhiShaderResourceBinding;
    QRhiShaderResourceBindings* cSrb = rhi->newShaderResourceBindings();
    cSrb->setBindings({
        SRB::bufferLoad     (0, SRB::ComputeStage, bCoeff),
        SRB::bufferLoad     (1, SRB::ComputeStage, bBasis),
        SRB::bufferLoad     (2, SRB::ComputeStage, bDirs),
        SRB::bufferLoad     (3, SRB::ComputeStage, bIdxStor),
        SRB::bufferLoad     (4, SRB::ComputeStage, bCenters),
        SRB::bufferLoadStore(5, SRB::ComputeStage, bRadii),
        SRB::bufferLoadStore(6, SRB::ComputeStage, bPos),
        SRB::bufferLoadStore(7, SRB::ComputeStage, bNor),
        SRB::uniformBuffer  (8, SRB::ComputeStage, bCUbo),
    });
    cSrb->create();

    QRhiShaderResourceBindings* gSrb = rhi->newShaderResourceBindings();
    gSrb->setBindings({
        SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, bRUbo),
        SRB::bufferLoad   (1, SRB::VertexStage, bPos),
        SRB::bufferLoad   (2, SRB::VertexStage, bNor),
        SRB::bufferLoad   (3, SRB::VertexStage, bDirs),
    });
    gSrb->create();

    QRhiComputePipeline* cPipe = rhi->newComputePipeline();
    cPipe->setShaderStage({ QRhiShaderStage::Compute, cs });
    cPipe->setShaderResourceBindings(cSrb);
    if (!cPipe->create()) { std::fprintf(stderr, "FAIL: compute pipeline\n"); return 4; }

    // offscreen target
    QRhiTexture* color = rhi->newTexture(QRhiTexture::RGBA8, QSize(W, H), 1,
                                         QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    color->create();
    QRhiRenderBuffer* ds = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(W, H), 1);
    ds->create();
    QRhiColorAttachment colorAtt(color);
    QRhiTextureRenderTargetDescription rtDesc(colorAtt);
    rtDesc.setDepthStencilBuffer(ds);
    QRhiTextureRenderTarget* rt = rhi->newTextureRenderTarget(rtDesc);
    QRhiRenderPassDescriptor* rp = rt->newCompatibleRenderPassDescriptor();
    rt->setRenderPassDescriptor(rp);
    rt->create();

    QRhiGraphicsPipeline* gPipe = rhi->newGraphicsPipeline();
    gPipe->setShaderStages({ { QRhiShaderStage::Vertex, vs }, { QRhiShaderStage::Fragment, fs } });
    QRhiVertexInputLayout emptyLayout;  // programmable vertex pulling: no vertex buffers
    gPipe->setVertexInputLayout(emptyLayout);
    gPipe->setShaderResourceBindings(gSrb);
    gPipe->setRenderPassDescriptor(rp);
    gPipe->setTopology(QRhiGraphicsPipeline::Triangles);
    gPipe->setCullMode(QRhiGraphicsPipeline::None);
    gPipe->setDepthTest(true);
    gPipe->setDepthWrite(true);
    gPipe->setDepthOp(QRhiGraphicsPipeline::Less);
    if (!gPipe->create()) { std::fprintf(stderr, "FAIL: graphics pipeline\n"); return 5; }

    // UBOs
    struct CUbo { int nVox, nDir, nTri, nCoeffs; float scale; int norm; int clampN; float eps; }
        cu{ nVox, nDir, nTri, nC, gp.scale, gp.normalizePerGlyph ? 1 : 0, gp.clampNegative ? 1 : 0, 1e-4f };

    QMatrix4x4 proj; proj.perspective(40.0f, float(W) / float(H), 0.01f * diag, 100.0f * diag);
    QMatrix4x4 view; view.lookAt(eye, center, QVector3D(0, 0, 1));
    QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix() * proj * view;
    struct RUbo { float mvp[16]; float light[4]; int nDir; int pad[3]; } ru{};
    std::memcpy(ru.mvp, mvp.constData(), 16 * sizeof(float));
    const QVector3D L = QVector3D(0.4f, -0.7f, 0.6f).normalized();
    ru.light[0] = L.x(); ru.light[1] = L.y(); ru.light[2] = L.z(); ru.light[3] = 0.0f;
    ru.nDir = nDir;

    // ---------------- one offscreen frame: compute + render + readbacks ----------------
    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) { std::fprintf(stderr, "FAIL: beginOffscreenFrame\n"); return 6; }

    QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(bCoeff, coeffsPacked.data());
    u->uploadStaticBuffer(bBasis, B.values.data());
    u->uploadStaticBuffer(bDirs, dirs4.data());
    u->uploadStaticBuffer(bIdxStor, idx.data());
    u->uploadStaticBuffer(bCenters, centers4.data());
    u->uploadStaticBuffer(bIdxDraw, idx.data());
    u->updateDynamicBuffer(bCUbo, 0, sizeof(cu), &cu);
    u->updateDynamicBuffer(bRUbo, 0, sizeof(ru), &ru);

    cb->beginComputePass(u);
    cb->setComputePipeline(cPipe);
    cb->setShaderResources(cSrb);
    cb->dispatch((nVox + 63) / 64, 1, 1);
    cb->endComputePass();

    cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), { 1.0f, 0 }, nullptr);
    cb->setGraphicsPipeline(gPipe);
    cb->setViewport(QRhiViewport(0, 0, W, H));
    cb->setShaderResources(gSrb);
    cb->setVertexInput(0, 0, nullptr, bIdxDraw, 0, QRhiCommandBuffer::IndexUInt32);
    cb->drawIndexed(quint32(idx.size()), quint32(nVox));
    cb->endPass();

    QRhiReadbackResult rbImg, rbPos;
    QRhiResourceUpdateBatch* u2 = rhi->nextResourceUpdateBatch();
    u2->readBackTexture(QRhiReadbackDescription(color), &rbImg);
    u2->readBackBuffer(bPos, 0, posBytes, &rbPos);
    cb->resourceUpdate(u2);
    rhi->endOffscreenFrame();

    // ---------------- verify GPU positions vs CPU oracle ----------------
    double maxAbs = 0.0; int worst = -1;
    if (rbPos.data.size() == int(posBytes)) {
        const float* gpos = reinterpret_cast<const float*>(rbPos.data.constData());
        for (std::size_t i = 0; i < oracle.positions.size(); ++i) {
            const Vec3 o = oracle.positions[i];
            const float gx = gpos[i * 4 + 0], gy = gpos[i * 4 + 1], gz = gpos[i * 4 + 2];
            const double d = std::sqrt(double(gx - o.x) * (gx - o.x) +
                                       double(gy - o.y) * (gy - o.y) +
                                       double(gz - o.z) * (gz - o.z));
            if (d > maxAbs) { maxAbs = d; worst = int(i); }
        }
        std::printf("positions compared: %zu   max |GPU-CPU oracle| = %.3e\n", oracle.positions.size(), maxAbs);
        if (worst >= 0)
            std::printf("worst @ %d: gpu=(%.5f,%.5f,%.5f) cpu=(%.5f,%.5f,%.5f)\n", worst,
                        double(gpos[worst * 4]), double(gpos[worst * 4 + 1]), double(gpos[worst * 4 + 2]),
                        double(oracle.positions[worst].x), double(oracle.positions[worst].y), double(oracle.positions[worst].z));
    } else {
        std::fprintf(stderr, "WARN: position readback size mismatch (%d vs %u)\n", rbPos.data.size(), posBytes);
    }

    // ---------------- save PNG ----------------
    QImage img(reinterpret_cast<const uchar*>(rbImg.data.constData()),
               rbImg.pixelSize.width(), rbImg.pixelSize.height(), QImage::Format_RGBA8888);
    QImage out = img.copy();
    if (rhi->isYUpInFramebuffer()) out = out.flipped(Qt::Vertical);
    const bool saved = out.save(outPng);

    const bool ok = saved && (maxAbs < 1e-4);
    std::printf("%s -> %s\n", saved ? "SAVED" : "SAVE FAILED", qPrintable(outPng));
    std::printf("\n%s\n", ok ? "GPU FUSION PASSED: GPU compute deform matches the CPU oracle and rendered."
                             : "GPU FUSION CHECK FAILED.");

    delete gPipe; delete cPipe; delete gSrb; delete cSrb;
    delete bIdxDraw; delete bRUbo; delete bCUbo; delete bNor; delete bPos; delete bRadii;
    delete bCenters; delete bIdxStor; delete bDirs; delete bBasis; delete bCoeff;
    delete rt; delete rp; delete ds; delete color; delete rhi;
    return ok ? 0 : 7;
}
