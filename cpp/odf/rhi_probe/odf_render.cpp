// First visible ODF render: load the demo fODF, build the verified CPU oracle glyph
// mesh (BuildGlyphs), and render it to a PNG with Qt RHI on Metal (offscreen, no
// window). This isolates and verifies the RHI graphics path — offscreen target,
// camera mvp, vertex layout, depth, two-sided shading, texture readback — using a
// mesh whose geometry is already proven. Fusing the GPU compute deform into the
// vertex stage is the next milestone.

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
static QVector3D qv(const Vec3& v) { return QVector3D(v.x, v.y, v.z); }

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const std::string niiPath =
        (argc > 1) ? argv[1] : "/Users/haoziqi/Documents/Finch-Viewer/dmri-explorer/data/odf.nii.gz";
    const QString shaderDir = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("/tmp/odf_rhi_build");
    const QString outPng    = (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("/tmp/odf_render.png");
    const int W = 900, H = 700;

    // ---------------- CPU: build the verified glyph mesh ----------------
    OdfVolume vol = LoadOdfNifti(niiPath);
    const ShOrder order = InferShOrder(std::size_t(vol.nCoeffs));
    const Icosphere ico = MakeIcosphere(3);  // 642 verts -> smooth glyphs
    const ShBasisMatrix B = BuildShBasisMatrix(ico.vertices, order);

    // voxel size from the affine columns -> glyph scale that fits a cell
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

    GlyphParams gp;
    gp.scale = 0.45f * voxSize;     // glyph radius <= ~half the voxel, so cells don't overlap
    gp.normalizePerGlyph = true;
    gp.clampNegative = true;
    const GlyphMesh mesh = BuildGlyphs(vol, ico, B, vox, gp);
    std::printf("volume %dx%dx%d nCoeffs=%d  voxSize=%.3f  glyphs=%zu  meshV=%zu tris=%zu\n",
                vol.dims[0], vol.dims[1], vol.dims[2], vol.nCoeffs, voxSize, vox.size(),
                mesh.vertexCount(), mesh.triangleCount());

    // interleave [pos3, normal3, color3]
    std::vector<float> vtx;
    vtx.reserve(mesh.positions.size() * 9);
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const Vec3 p = mesh.positions[i], n = mesh.normals[i], c = mesh.colors[i];
        vtx.insert(vtx.end(), {p.x, p.y, p.z, n.x, n.y, n.z, c.x, c.y, c.z});
    }
    const std::vector<unsigned>& idx = mesh.indices;

    // camera framed on the world AABB (padded for the glyph radii sticking out)
    const QVector3D center = qv(vol.worldBounds.center());
    const Vec3 ext = vol.worldBounds.extent();
    const float diag = std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z) + 2.0f * gp.scale;
    const QVector3D eye = center + QVector3D(0.9f, -1.0f, 0.6f).normalized() * (1.6f * diag + 3.0f * gp.scale);
    const QVector3D up(0, 0, 1);

    // ---------------- RHI: Metal offscreen ----------------
    QRhiMetalInitParams params;
    QRhi* rhi = QRhi::create(QRhi::Metal, &params);
    if (!rhi) { std::fprintf(stderr, "FAIL: QRhi::create(Metal)\n"); return 2; }
    std::printf("gpu %s (%s)\n", rhi->driverInfo().deviceName.constData(), rhi->backendName());

    QShader vs = loadShader(shaderDir + "/glyph.vert.qsb");
    QShader fs = loadShader(shaderDir + "/glyph.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) { std::fprintf(stderr, "FAIL: load glyph shaders from %s\n", qPrintable(shaderDir)); return 3; }

    QRhiTexture* color = rhi->newTexture(QRhiTexture::RGBA8, QSize(W, H), 1,
                                         QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    color->create();
    QRhiRenderBuffer* ds = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(W, H), 1);
    ds->create();
    QRhiColorAttachment colorAtt(color);  // named var: avoid the most-vexing-parse
    QRhiTextureRenderTargetDescription rtDesc(colorAtt);
    rtDesc.setDepthStencilBuffer(ds);
    QRhiTextureRenderTarget* rt = rhi->newTextureRenderTarget(rtDesc);
    QRhiRenderPassDescriptor* rp = rt->newCompatibleRenderPassDescriptor();
    rt->setRenderPassDescriptor(rp);
    rt->create();

    QRhiBuffer* vbuf = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(vtx.size() * sizeof(float)));
    vbuf->create();
    QRhiBuffer* ibuf = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, quint32(idx.size() * sizeof(unsigned)));
    ibuf->create();
    QRhiBuffer* ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    ubuf->create();

    using SRB = QRhiShaderResourceBinding;
    QRhiShaderResourceBindings* srb = rhi->newShaderResourceBindings();
    srb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, ubuf) });
    srb->create();

    QRhiGraphicsPipeline* ps = rhi->newGraphicsPipeline();
    ps->setShaderStages({ { QRhiShaderStage::Vertex, vs }, { QRhiShaderStage::Fragment, fs } });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ QRhiVertexInputBinding(9 * sizeof(float)) });
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)),
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float3, 6 * sizeof(float)),
    });
    ps->setVertexInputLayout(inputLayout);
    ps->setShaderResourceBindings(srb);
    ps->setRenderPassDescriptor(rp);
    ps->setTopology(QRhiGraphicsPipeline::Triangles);
    ps->setCullMode(QRhiGraphicsPipeline::None);  // first image: don't trust winding yet
    ps->setDepthTest(true);
    ps->setDepthWrite(true);
    ps->setDepthOp(QRhiGraphicsPipeline::Less);
    if (!ps->create()) { std::fprintf(stderr, "FAIL: graphics pipeline create\n"); return 4; }

    // mvp (model = identity; positions already world-space). clipSpaceCorrMatrix maps
    // QMatrix4x4's GL convention to the active backend (Metal).
    QMatrix4x4 proj;
    proj.perspective(40.0f, float(W) / float(H), 0.01f * diag, 100.0f * diag);
    QMatrix4x4 view;
    view.lookAt(eye, center, up);
    QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix() * proj * view;

    struct Ubo { float mvp[16]; float light[4]; } ud{};
    memcpy(ud.mvp, mvp.constData(), 16 * sizeof(float));
    const QVector3D L = QVector3D(0.4f, -0.7f, 0.6f).normalized();
    ud.light[0] = L.x(); ud.light[1] = L.y(); ud.light[2] = L.z(); ud.light[3] = 0.0f;

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) { std::fprintf(stderr, "FAIL: beginOffscreenFrame\n"); return 5; }

    QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(vbuf, vtx.data());
    u->uploadStaticBuffer(ibuf, idx.data());
    u->updateDynamicBuffer(ubuf, 0, sizeof(ud), &ud);

    cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(ps);
    cb->setViewport(QRhiViewport(0, 0, W, H));
    cb->setShaderResources(srb);
    const QRhiCommandBuffer::VertexInput vin(vbuf, 0);
    cb->setVertexInput(0, 1, &vin, ibuf, 0, QRhiCommandBuffer::IndexUInt32);
    cb->drawIndexed(quint32(idx.size()));
    cb->endPass();

    QRhiReadbackResult rb;
    QRhiResourceUpdateBatch* u2 = rhi->nextResourceUpdateBatch();
    u2->readBackTexture(QRhiReadbackDescription(color), &rb);
    cb->resourceUpdate(u2);
    rhi->endOffscreenFrame();

    QImage img(reinterpret_cast<const uchar*>(rb.data.constData()),
               rb.pixelSize.width(), rb.pixelSize.height(), QImage::Format_RGBA8888);
    QImage out = img.copy();
    if (rhi->isYUpInFramebuffer()) out = out.flipped(Qt::Vertical);
    const bool saved = out.save(outPng);
    std::printf("%s -> %s (%dx%d)\n", saved ? "SAVED" : "FAILED TO SAVE", qPrintable(outPng),
                rb.pixelSize.width(), rb.pixelSize.height());

    delete ps; delete srb; delete ubuf; delete ibuf; delete vbuf;
    delete rt; delete rp; delete ds; delete color; delete rhi;
    return saved ? 0 : 6;
}
