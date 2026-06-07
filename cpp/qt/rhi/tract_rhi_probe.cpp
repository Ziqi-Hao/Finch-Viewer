// Migration increments 1-3: render streamlines + FA background slices + the 3D
// selection box (wireframe + handles) with Qt RHI (Metal), offscreen -> PNG.
// Reuses the renderer-agnostic core unchanged (trk_io, tractogram_store,
// display_geometry, nifti_io, OrbitCamera, Bounds); only the GPU backend is new.
// Headless dev/verify vehicle; the QRhiWidget viewport (with mouse pick/drag) wraps
// this later. NOTE (Metal): line width fixed 1px; point size set via gl_PointSize.

#include "trk_io.hpp"
#include "tractogram_store.hpp"
#include "display_geometry.hpp"
#include "nifti_io.hpp"
#include "render_math.hpp"
#include "bounds.hpp"

#include <rhi/qrhi.h>
#include <QGuiApplication>
#include <QFile>
#include <QImage>
#include <QMatrix4x4>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tracto;

static QShader loadShader(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// ---- geometry helpers (all emit interleaved [pos.xyz, color.rgb]) ----
static void PushLine(std::vector<float>& o, Vec3 a, Vec3 b, std::array<float, 3> c) {
    o.insert(o.end(), { a.x, a.y, a.z, c[0], c[1], c[2], b.x, b.y, b.z, c[0], c[1], c[2] });
}
static Bounds PlaceBox(const Bounds& b, double frac) {
    Bounds box;
    for (int a = 0; a < 3; ++a) {
        const double c = 0.5 * (b.v[a * 2] + b.v[a * 2 + 1]);
        const double h = 0.5 * frac * (b.v[a * 2 + 1] - b.v[a * 2]);
        box.v[a * 2] = c - h;
        box.v[a * 2 + 1] = c + h;
    }
    return box;
}
static std::vector<float> BuildBoxEdges(const Bounds& box, std::array<float, 3> col) {
    const Vec3 lo{ float(box.v[0]), float(box.v[2]), float(box.v[4]) };
    const Vec3 hi{ float(box.v[1]), float(box.v[3]), float(box.v[5]) };
    const Vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                        { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    const int e[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
    std::vector<float> o;
    for (const auto& ed : e) PushLine(o, c[ed[0]], c[ed[1]], col);
    return o;
}
static std::vector<float> BuildHandles(const Bounds& box) {
    const float cx = 0.5f * float(box.v[0] + box.v[1]);
    const float cy = 0.5f * float(box.v[2] + box.v[3]);
    const float cz = 0.5f * float(box.v[4] + box.v[5]);
    const std::array<float, 3> orange{ 1.0f, 0.55f, 0.1f }, green{ 0.2f, 1.0f, 0.3f };
    struct H { Vec3 p; std::array<float, 3> c; };
    const H hs[7] = {
        { { cx, cy, cz }, orange },
        { { float(box.v[0]), cy, cz }, green }, { { float(box.v[1]), cy, cz }, green },
        { { cx, float(box.v[2]), cz }, green }, { { cx, float(box.v[3]), cz }, green },
        { { cx, cy, float(box.v[4]) }, green }, { { cx, cy, float(box.v[5]) }, green },
    };
    std::vector<float> o;
    for (const auto& h : hs) o.insert(o.end(), { h.p.x, h.p.y, h.p.z, h.c[0], h.c[1], h.c[2] });
    return o;
}
static std::vector<float> BuildSliceQuads(const int dims[3]) {
    const float mx = float(dims[0] / 2), my = float(dims[1] / 2), mz = float(dims[2] / 2);
    const float ex = float(dims[0] - 1), ey = float(dims[1] - 1), ez = float(dims[2] - 1);
    std::vector<float> s;
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        for (const Vec3& p : { a, b, c, a, c, d }) s.insert(s.end(), { p.x, p.y, p.z });
    };
    quad({ mx, 0, 0 }, { mx, ey, 0 }, { mx, ey, ez }, { mx, 0, ez });
    quad({ 0, my, 0 }, { ex, my, 0 }, { ex, my, ez }, { 0, my, ez });
    quad({ 0, 0, mz }, { ex, 0, mz }, { ex, ey, mz }, { 0, ey, mz });
    return s;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const std::string trkPath = (argc > 1) ? argv[1] : "/Users/haoziqi/Documents/Finch-Viewer/data/ORedited.trk";
    const std::string faPath  = (argc > 2) ? argv[2] : "/Users/haoziqi/Documents/Finch-Viewer/data/SUBG08_tissue_FA_aggressive.nii.gz";
    const QString shaderDir   = (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("/tmp/tract_rhi_build");
    const QString outPng      = (argc > 4) ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("/tmp/tract_rhi.png");
    const int displayN        = (argc > 5) ? std::atoi(argv[5]) : 12000;
    const int W = 1000, H = 800;

    // ---- CPU: load streamlines + FA, build display geometry + selection box ----
    std::printf("loading %s ...\n", trkPath.c_str());
    TractogramStore store;
    store.streamlines = LoadTrk(trkPath, store.header);
    BuildSoA(store);
    std::vector<uint8_t> alive(store.StreamlineCount(), 1);
    const LineGeometry geo = BuildDisplayLineGeometry(store, alive, displayN, 2, 0);
    std::printf("streamlines=%zu  display verts=%zu\n", store.StreamlineCount(), geo.VertexCount());

    Volume fa;
    bool haveFa = false;
    try { fa = LoadNifti(faPath); haveFa = !fa.Empty(); }
    catch (const std::exception& e) { std::fprintf(stderr, "FA load failed (%s); tracts only\n", e.what()); }
    std::vector<float> sliceQuads;
    if (haveFa) sliceQuads = BuildSliceQuads(fa.dims);

    const Bounds box = PlaceBox(geo.bounds, 0.6);
    const std::vector<float> boxVerts = BuildBoxEdges(box, { 0.1f, 0.9f, 0.95f });   // cyan
    const std::vector<float> handleVerts = BuildHandles(box);

    OrbitCamera cam;
    cam.Frame(geo.bounds);
    const Mat4 vpRow = cam.ViewProj(float(W) / float(H));

    // ---- RHI Metal offscreen ----
    QRhiMetalInitParams params;
    QRhi* rhi = QRhi::create(QRhi::Metal, &params);
    if (!rhi) { std::fprintf(stderr, "FAIL: QRhi::create(Metal)\n"); return 2; }
    std::printf("gpu %s (%s)\n", rhi->driverInfo().deviceName.constData(), rhi->backendName());

    const QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix() * QMatrix4x4(vpRow.m);
    float mvpCol[16];
    std::memcpy(mvpCol, mvp.constData(), sizeof(mvpCol));

    QShader lineVs = loadShader(shaderDir + "/line.vert.qsb");
    QShader lineFs = loadShader(shaderDir + "/line.frag.qsb");
    QShader pointVs = loadShader(shaderDir + "/point.vert.qsb");
    QShader sliceVs = loadShader(shaderDir + "/slice.vert.qsb");
    QShader sliceFs = loadShader(shaderDir + "/slice.frag.qsb");
    if (!lineVs.isValid() || !lineFs.isValid() || !pointVs.isValid() ||
        (haveFa && (!sliceVs.isValid() || !sliceFs.isValid()))) {
        std::fprintf(stderr, "FAIL: load shaders from %s\n", qPrintable(shaderDir)); return 3;
    }

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

    using SRB = QRhiShaderResourceBinding;

    // shared [pos.xyz, color.rgb] vertex layout (lines, box, handle points)
    QRhiVertexInputLayout pcLayout;
    pcLayout.setBindings({ QRhiVertexInputBinding(6 * sizeof(float)) });
    pcLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)),
    });

    // --- line/box program (mvp UBO) ---
    QRhiBuffer* lineVbo = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(geo.vertices.size() * sizeof(float)));
    lineVbo->create();
    QRhiBuffer* boxVbo = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(boxVerts.size() * sizeof(float)));
    boxVbo->create();
    QRhiBuffer* lineUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    lineUbo->create();
    QRhiShaderResourceBindings* lineSrb = rhi->newShaderResourceBindings();
    lineSrb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage, lineUbo) });
    lineSrb->create();

    QRhiGraphicsPipeline* linePs = rhi->newGraphicsPipeline();
    linePs->setShaderStages({ { QRhiShaderStage::Vertex, lineVs }, { QRhiShaderStage::Fragment, lineFs } });
    linePs->setVertexInputLayout(pcLayout);
    linePs->setShaderResourceBindings(lineSrb);
    linePs->setRenderPassDescriptor(rp);
    linePs->setTopology(QRhiGraphicsPipeline::Lines);
    linePs->setDepthTest(true);
    linePs->setDepthWrite(true);
    linePs->setDepthOp(QRhiGraphicsPipeline::Less);
    if (!linePs->create()) { std::fprintf(stderr, "FAIL: line pipeline\n"); return 4; }

    // --- handle points program (mvp + point size; overlay, depth off) ---
    QRhiBuffer* handleVbo = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(handleVerts.size() * sizeof(float)));
    handleVbo->create();
    QRhiBuffer* pointUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    pointUbo->create();
    QRhiShaderResourceBindings* pointSrb = rhi->newShaderResourceBindings();
    pointSrb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage, pointUbo) });
    pointSrb->create();
    QRhiGraphicsPipeline* pointPs = rhi->newGraphicsPipeline();
    pointPs->setShaderStages({ { QRhiShaderStage::Vertex, pointVs }, { QRhiShaderStage::Fragment, lineFs } });
    pointPs->setVertexInputLayout(pcLayout);
    pointPs->setShaderResourceBindings(pointSrb);
    pointPs->setRenderPassDescriptor(rp);
    pointPs->setTopology(QRhiGraphicsPipeline::Points);
    pointPs->setDepthTest(false);
    pointPs->setDepthWrite(false);
    if (!pointPs->create()) { std::fprintf(stderr, "FAIL: point pipeline\n"); return 5; }

    // --- FA slice program (3D texture, blended) ---
    QRhiBuffer* sliceVbo = nullptr; QRhiBuffer* sliceUbo = nullptr;
    QRhiTexture* volTex = nullptr; QRhiSampler* sampler = nullptr;
    QRhiShaderResourceBindings* sliceSrb = nullptr; QRhiGraphicsPipeline* slicePs = nullptr;
    if (haveFa) {
        sliceVbo = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(sliceQuads.size() * sizeof(float)));
        sliceVbo->create();
        sliceUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
        sliceUbo->create();
        volTex = rhi->newTexture(QRhiTexture::R32F, fa.dims[0], fa.dims[1], fa.dims[2], 1, QRhiTexture::ThreeDimensional);
        if (!volTex->create()) { std::fprintf(stderr, "FAIL: 3D texture\n"); return 6; }
        sampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        sampler->create();
        sliceSrb = rhi->newShaderResourceBindings();
        sliceSrb->setBindings({
            SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, sliceUbo),
            SRB::sampledTexture(1, SRB::FragmentStage, volTex, sampler),
        });
        sliceSrb->create();
        slicePs = rhi->newGraphicsPipeline();
        slicePs->setShaderStages({ { QRhiShaderStage::Vertex, sliceVs }, { QRhiShaderStage::Fragment, sliceFs } });
        QRhiVertexInputLayout sliceLayout;
        sliceLayout.setBindings({ QRhiVertexInputBinding(3 * sizeof(float)) });
        sliceLayout.setAttributes({ QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0) });
        slicePs->setVertexInputLayout(sliceLayout);
        slicePs->setShaderResourceBindings(sliceSrb);
        slicePs->setRenderPassDescriptor(rp);
        slicePs->setTopology(QRhiGraphicsPipeline::Triangles);
        slicePs->setCullMode(QRhiGraphicsPipeline::None);
        slicePs->setDepthTest(true);
        slicePs->setDepthWrite(false);
        QRhiGraphicsPipeline::TargetBlend tb;
        tb.enable = true;
        tb.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        tb.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        tb.srcAlpha = QRhiGraphicsPipeline::One;
        tb.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        slicePs->setTargetBlends({ tb });
        if (!slicePs->create()) { std::fprintf(stderr, "FAIL: slice pipeline\n"); return 7; }
    }

    // UBO contents
    struct PointUbo { float mvp[16]; float params[4]; } pu{};
    std::memcpy(pu.mvp, mvpCol, sizeof(pu.mvp));
    pu.params[0] = 13.0f;  // handle point size (px)

    struct SliceUbo { float mvp[16]; float voxToWorld[16]; float invDims[4]; float valueParams[4]; } su{};
    if (haveFa) {
        std::memcpy(su.mvp, mvpCol, sizeof(su.mvp));
        QMatrix4x4 v2w(fa.voxelToWorld.m);
        std::memcpy(su.voxToWorld, v2w.constData(), sizeof(su.voxToWorld));
        su.invDims[0] = 1.0f / fa.dims[0]; su.invDims[1] = 1.0f / fa.dims[1]; su.invDims[2] = 1.0f / fa.dims[2];
        su.valueParams[0] = fa.valueMin;
        su.valueParams[1] = std::max(1e-6f, fa.valueMax - fa.valueMin);
    }

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) { std::fprintf(stderr, "FAIL: beginOffscreenFrame\n"); return 8; }

    QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(lineVbo, geo.vertices.data());
    u->uploadStaticBuffer(boxVbo, boxVerts.data());
    u->uploadStaticBuffer(handleVbo, handleVerts.data());
    u->updateDynamicBuffer(lineUbo, 0, sizeof(mvpCol), mvpCol);
    u->updateDynamicBuffer(pointUbo, 0, sizeof(pu), &pu);
    if (haveFa) {
        u->uploadStaticBuffer(sliceVbo, sliceQuads.data());
        u->updateDynamicBuffer(sliceUbo, 0, sizeof(su), &su);
        const int nx = fa.dims[0], ny = fa.dims[1], nz = fa.dims[2];
        std::vector<QRhiTextureUploadEntry> entries; entries.reserve(nz);
        for (int z = 0; z < nz; ++z) {
            QRhiTextureSubresourceUploadDescription sub(fa.data.data() + std::size_t(z) * nx * ny, quint32(nx) * ny * sizeof(float));
            sub.setDataStride(quint32(nx) * sizeof(float));
            entries.emplace_back(z, 0, sub);
        }
        QRhiTextureUploadDescription desc;
        desc.setEntries(entries.begin(), entries.end());
        u->uploadTexture(volTex, desc);
    }

    cb->beginPass(rt, QColor::fromRgbF(0.06, 0.066, 0.082), { 1.0f, 0 }, u);
    // opaque: streamlines + selection-box wireframe (same line pipeline)
    cb->setGraphicsPipeline(linePs);
    cb->setViewport(QRhiViewport(0, 0, W, H));
    cb->setShaderResources(lineSrb);
    const QRhiCommandBuffer::VertexInput lineVin(lineVbo, 0);
    cb->setVertexInput(0, 1, &lineVin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
    cb->draw(quint32(geo.VertexCount()));
    const QRhiCommandBuffer::VertexInput boxVin(boxVbo, 0);
    cb->setVertexInput(0, 1, &boxVin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
    cb->draw(quint32(boxVerts.size() / 6));
    // translucent FA slices
    if (haveFa) {
        cb->setGraphicsPipeline(slicePs);
        cb->setShaderResources(sliceSrb);
        const QRhiCommandBuffer::VertexInput sliceVin(sliceVbo, 0);
        cb->setVertexInput(0, 1, &sliceVin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
        cb->draw(quint32(sliceQuads.size() / 3));
    }
    // overlay: handle points (depth off, always visible)
    cb->setGraphicsPipeline(pointPs);
    cb->setShaderResources(pointSrb);
    const QRhiCommandBuffer::VertexInput handleVin(handleVbo, 0);
    cb->setVertexInput(0, 1, &handleVin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
    cb->draw(quint32(handleVerts.size() / 6));
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
    std::printf("%s -> %s (%dx%d)  tracts + box%s\n", saved ? "SAVED" : "SAVE FAILED",
                qPrintable(outPng), rb.pixelSize.width(), rb.pixelSize.height(), haveFa ? " + FA" : "");

    delete slicePs; delete sliceSrb; delete sampler; delete volTex; delete sliceUbo; delete sliceVbo;
    delete pointPs; delete pointSrb; delete pointUbo; delete handleVbo;
    delete linePs; delete lineSrb; delete lineUbo; delete boxVbo; delete lineVbo;
    delete rt; delete rp; delete ds; delete color; delete rhi;
    return saved ? 0 : 9;
}
