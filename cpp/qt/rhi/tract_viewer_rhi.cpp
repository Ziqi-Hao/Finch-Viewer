// Migration increment 5: interactive QRhiWidget (Metal) tractography viewport —
// streamlines + FA slices + selection box/handles, with an orbit camera (left-drag
// rotate, right/middle-drag pan, wheel zoom). This is the migrated viewport in its
// interactive form; box handle pick/drag (increment 5b) and the coordinated cutover
// into main_window (increment 6) come next. Reuses the renderer-agnostic core
// (trk_io, tractogram_store, display_geometry, nifti_io, OrbitCamera) unchanged.
//
// Run:    tract_viewer_rhi <trk> <fa.nii.gz> <shaderDir>
// Verify: tract_viewer_rhi <trk> <fa> <shaderDir> --screenshot out.png

#include "trk_io.hpp"
#include "tractogram_store.hpp"
#include "display_geometry.hpp"
#include "nifti_io.hpp"
#include "render_math.hpp"
#include "bounds.hpp"

#include <rhi/qrhi.h>
#include <QRhiWidget>
#include <QApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>

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
static void PushLine(std::vector<float>& o, Vec3 a, Vec3 b, std::array<float, 3> c) {
    o.insert(o.end(), { a.x, a.y, a.z, c[0], c[1], c[2], b.x, b.y, b.z, c[0], c[1], c[2] });
}
static Bounds PlaceBox(const Bounds& b, double frac) {
    Bounds box;
    for (int a = 0; a < 3; ++a) {
        const double c = 0.5 * (b.v[a * 2] + b.v[a * 2 + 1]);
        const double h = 0.5 * frac * (b.v[a * 2 + 1] - b.v[a * 2]);
        box.v[a * 2] = c - h; box.v[a * 2 + 1] = c + h;
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
    const float cx = 0.5f * float(box.v[0] + box.v[1]), cy = 0.5f * float(box.v[2] + box.v[3]), cz = 0.5f * float(box.v[4] + box.v[5]);
    const std::array<float, 3> orange{ 1.0f, 0.55f, 0.1f }, green{ 0.2f, 1.0f, 0.3f };
    struct H { Vec3 p; std::array<float, 3> c; };
    const H hs[7] = { { { cx, cy, cz }, orange },
        { { float(box.v[0]), cy, cz }, green }, { { float(box.v[1]), cy, cz }, green },
        { { cx, float(box.v[2]), cz }, green }, { { cx, float(box.v[3]), cz }, green },
        { { cx, cy, float(box.v[4]) }, green }, { { cx, cy, float(box.v[5]) }, green } };
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
    quad({ mx,0,0 }, { mx,ey,0 }, { mx,ey,ez }, { mx,0,ez });
    quad({ 0,my,0 }, { ex,my,0 }, { ex,my,ez }, { 0,my,ez });
    quad({ 0,0,mz }, { ex,0,mz }, { ex,ey,mz }, { 0,ey,mz });
    return s;
}

class TractViewerRhi : public QRhiWidget {
public:
    TractViewerRhi(const std::string& trk, const std::string& fa, const QString& shaderDir)
        : m_shaderDir(shaderDir) {
        setApi(QRhiWidget::Api::Metal);
        setSampleCount(4);
        load(trk, fa);
    }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (m_rhi != rhi()) { releaseAll(); m_rhi = rhi(); }
        if (!m_linePs) createResources();
    }

    void render(QRhiCommandBuffer* cb) override {
        QRhiRenderTarget* rt = renderTarget();
        const QSize px = rt->pixelSize();
        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();

        if (!m_uploaded) {
            u->uploadStaticBuffer(m_lineVbo, m_line.data());
            u->uploadStaticBuffer(m_boxVbo, m_box.data());
            u->uploadStaticBuffer(m_handleVbo, m_handles.data());
            if (m_haveFa) {
                u->uploadStaticBuffer(m_sliceVbo, m_slices.data());
                const int nx = m_fa.dims[0], ny = m_fa.dims[1], nz = m_fa.dims[2];
                std::vector<QRhiTextureUploadEntry> ents; ents.reserve(nz);
                for (int z = 0; z < nz; ++z) {
                    QRhiTextureSubresourceUploadDescription sub(m_fa.data.data() + std::size_t(z) * nx * ny, quint32(nx) * ny * sizeof(float));
                    sub.setDataStride(quint32(nx) * sizeof(float));
                    ents.emplace_back(z, 0, sub);
                }
                QRhiTextureUploadDescription d; d.setEntries(ents.begin(), ents.end());
                u->uploadTexture(m_volTex, d);
            }
            m_uploaded = true;
        }

        // per-frame camera matrices
        const float aspect = px.height() ? float(px.width()) / float(px.height()) : 1.0f;
        const QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix() * QMatrix4x4(m_cam.ViewProj(aspect).m);
        float mvpCol[16]; std::memcpy(mvpCol, mvp.constData(), sizeof(mvpCol));
        u->updateDynamicBuffer(m_lineUbo, 0, sizeof(mvpCol), mvpCol);
        struct PointUbo { float mvp[16]; float params[4]; } pu{};
        std::memcpy(pu.mvp, mvpCol, sizeof(pu.mvp)); pu.params[0] = 13.0f;
        u->updateDynamicBuffer(m_pointUbo, 0, sizeof(pu), &pu);
        if (m_haveFa) {
            struct SliceUbo { float mvp[16]; float voxToWorld[16]; float invDims[4]; float valueParams[4]; } su{};
            std::memcpy(su.mvp, mvpCol, sizeof(su.mvp));
            QMatrix4x4 v2w(m_fa.voxelToWorld.m); std::memcpy(su.voxToWorld, v2w.constData(), sizeof(su.voxToWorld));
            su.invDims[0] = 1.0f / m_fa.dims[0]; su.invDims[1] = 1.0f / m_fa.dims[1]; su.invDims[2] = 1.0f / m_fa.dims[2];
            su.valueParams[0] = m_fa.valueMin; su.valueParams[1] = std::max(1e-6f, m_fa.valueMax - m_fa.valueMin);
            u->updateDynamicBuffer(m_sliceUbo, 0, sizeof(su), &su);
        }

        cb->beginPass(rt, QColor::fromRgbF(0.06, 0.066, 0.082), { 1.0f, 0 }, u);
        cb->setGraphicsPipeline(m_linePs);
        cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
        cb->setShaderResources(m_lineSrb);
        QRhiCommandBuffer::VertexInput vin(m_lineVbo, 0);
        cb->setVertexInput(0, 1, &vin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
        cb->draw(quint32(m_line.size() / 6));
        QRhiCommandBuffer::VertexInput bvin(m_boxVbo, 0);
        cb->setVertexInput(0, 1, &bvin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
        cb->draw(quint32(m_box.size() / 6));
        if (m_haveFa) {
            cb->setGraphicsPipeline(m_slicePs);
            cb->setShaderResources(m_sliceSrb);
            QRhiCommandBuffer::VertexInput svin(m_sliceVbo, 0);
            cb->setVertexInput(0, 1, &svin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
            cb->draw(quint32(m_slices.size() / 3));
        }
        cb->setGraphicsPipeline(m_pointPs);
        cb->setShaderResources(m_pointSrb);
        QRhiCommandBuffer::VertexInput hvin(m_handleVbo, 0);
        cb->setVertexInput(0, 1, &hvin, nullptr, 0, QRhiCommandBuffer::IndexUInt32);
        cb->draw(quint32(m_handles.size() / 6));
        cb->endPass();
    }

    void releaseResources() override { releaseAll(); }

    void mousePressEvent(QMouseEvent* e) override { m_last = e->position().toPoint(); m_btn = e->button(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QPoint p = e->position().toPoint();
        const double dx = p.x() - m_last.x(), dy = p.y() - m_last.y();
        if (e->buttons() & Qt::LeftButton) m_cam.Rotate(dx, dy);
        else if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) m_cam.Pan(dx, dy);
        m_last = p;
        update();
    }
    void wheelEvent(QWheelEvent* e) override { m_cam.Zoom(e->angleDelta().y() / 120.0); update(); }

private:
    void load(const std::string& trk, const std::string& fa) {
        TractogramStore store;
        store.streamlines = LoadTrk(trk, store.header);
        BuildSoA(store);
        std::vector<uint8_t> alive(store.StreamlineCount(), 1);
        const LineGeometry geo = BuildDisplayLineGeometry(store, alive, 12000, 2, 0);
        m_line = geo.vertices;
        std::printf("streamlines=%zu display verts=%zu\n", store.StreamlineCount(), geo.VertexCount());
        try { m_fa = LoadNifti(fa); m_haveFa = !m_fa.Empty(); }
        catch (const std::exception& ex) { std::fprintf(stderr, "FA load failed (%s)\n", ex.what()); }
        if (m_haveFa) m_slices = BuildSliceQuads(m_fa.dims);
        const Bounds box = PlaceBox(geo.bounds, 0.6);
        m_box = BuildBoxEdges(box, { 0.1f, 0.9f, 0.95f });
        m_handles = BuildHandles(box);
        m_cam.Frame(geo.bounds);
    }

    void createResources() {
        m_lineVs = loadShader(m_shaderDir + "/line.vert.qsb");
        m_lineFs = loadShader(m_shaderDir + "/line.frag.qsb");
        m_pointVs = loadShader(m_shaderDir + "/point.vert.qsb");
        m_sliceVs = loadShader(m_shaderDir + "/slice.vert.qsb");
        m_sliceFs = loadShader(m_shaderDir + "/slice.frag.qsb");

        auto buf = [&](QRhiBuffer::Type t, QRhiBuffer::UsageFlags us, quint32 sz) {
            QRhiBuffer* b = m_rhi->newBuffer(t, us, sz); b->create(); return b;
        };
        m_lineVbo = buf(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(m_line.size() * sizeof(float)));
        m_boxVbo = buf(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(m_box.size() * sizeof(float)));
        m_handleVbo = buf(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(m_handles.size() * sizeof(float)));
        m_lineUbo = buf(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
        m_pointUbo = buf(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);

        using SRB = QRhiShaderResourceBinding;
        QRhiVertexInputLayout pc;
        pc.setBindings({ QRhiVertexInputBinding(6 * sizeof(float)) });
        pc.setAttributes({ QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                           QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)) });

        m_lineSrb = m_rhi->newShaderResourceBindings();
        m_lineSrb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage, m_lineUbo) });
        m_lineSrb->create();
        m_linePs = m_rhi->newGraphicsPipeline();
        m_linePs->setShaderStages({ { QRhiShaderStage::Vertex, m_lineVs }, { QRhiShaderStage::Fragment, m_lineFs } });
        m_linePs->setVertexInputLayout(pc);
        m_linePs->setShaderResourceBindings(m_lineSrb);
        m_linePs->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_linePs->setSampleCount(renderTarget()->sampleCount());
        m_linePs->setTopology(QRhiGraphicsPipeline::Lines);
        m_linePs->setDepthTest(true); m_linePs->setDepthWrite(true);
        m_linePs->create();

        m_pointSrb = m_rhi->newShaderResourceBindings();
        m_pointSrb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage, m_pointUbo) });
        m_pointSrb->create();
        m_pointPs = m_rhi->newGraphicsPipeline();
        m_pointPs->setShaderStages({ { QRhiShaderStage::Vertex, m_pointVs }, { QRhiShaderStage::Fragment, m_lineFs } });
        m_pointPs->setVertexInputLayout(pc);
        m_pointPs->setShaderResourceBindings(m_pointSrb);
        m_pointPs->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_pointPs->setSampleCount(renderTarget()->sampleCount());
        m_pointPs->setTopology(QRhiGraphicsPipeline::Points);
        m_pointPs->setDepthTest(false); m_pointPs->setDepthWrite(false);
        m_pointPs->create();

        if (m_haveFa) {
            m_sliceVbo = buf(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(m_slices.size() * sizeof(float)));
            m_sliceUbo = buf(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
            m_volTex = m_rhi->newTexture(QRhiTexture::R32F, m_fa.dims[0], m_fa.dims[1], m_fa.dims[2], 1, QRhiTexture::ThreeDimensional);
            m_volTex->create();
            m_sampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
            m_sampler->create();
            m_sliceSrb = m_rhi->newShaderResourceBindings();
            m_sliceSrb->setBindings({ SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, m_sliceUbo),
                                      SRB::sampledTexture(1, SRB::FragmentStage, m_volTex, m_sampler) });
            m_sliceSrb->create();
            m_slicePs = m_rhi->newGraphicsPipeline();
            m_slicePs->setShaderStages({ { QRhiShaderStage::Vertex, m_sliceVs }, { QRhiShaderStage::Fragment, m_sliceFs } });
            QRhiVertexInputLayout sl;
            sl.setBindings({ QRhiVertexInputBinding(3 * sizeof(float)) });
            sl.setAttributes({ QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0) });
            m_slicePs->setVertexInputLayout(sl);
            m_slicePs->setShaderResourceBindings(m_sliceSrb);
            m_slicePs->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            m_slicePs->setSampleCount(renderTarget()->sampleCount());
            m_slicePs->setTopology(QRhiGraphicsPipeline::Triangles);
            m_slicePs->setCullMode(QRhiGraphicsPipeline::None);
            m_slicePs->setDepthTest(true); m_slicePs->setDepthWrite(false);
            QRhiGraphicsPipeline::TargetBlend tb;
            tb.enable = true; tb.srcColor = QRhiGraphicsPipeline::SrcAlpha; tb.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            tb.srcAlpha = QRhiGraphicsPipeline::One; tb.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            m_slicePs->setTargetBlends({ tb });
            m_slicePs->create();
        }
        m_uploaded = false;
    }

    void releaseAll() {
        for (QRhiResource** r : { (QRhiResource**)&m_slicePs, (QRhiResource**)&m_sliceSrb, (QRhiResource**)&m_sampler,
                                  (QRhiResource**)&m_volTex, (QRhiResource**)&m_sliceUbo, (QRhiResource**)&m_sliceVbo,
                                  (QRhiResource**)&m_pointPs, (QRhiResource**)&m_pointSrb, (QRhiResource**)&m_pointUbo,
                                  (QRhiResource**)&m_linePs, (QRhiResource**)&m_lineSrb, (QRhiResource**)&m_lineUbo,
                                  (QRhiResource**)&m_handleVbo, (QRhiResource**)&m_boxVbo, (QRhiResource**)&m_lineVbo }) {
            if (*r) { (*r)->destroy(); delete *r; *r = nullptr; }
        }
    }

    QString m_shaderDir;
    std::vector<float> m_line, m_box, m_handles, m_slices;
    Volume m_fa; bool m_haveFa = false;
    OrbitCamera m_cam;
    QPoint m_last; Qt::MouseButton m_btn = Qt::NoButton;

    QRhi* m_rhi = nullptr;
    QRhiBuffer *m_lineVbo = nullptr, *m_boxVbo = nullptr, *m_handleVbo = nullptr, *m_lineUbo = nullptr,
               *m_pointUbo = nullptr, *m_sliceVbo = nullptr, *m_sliceUbo = nullptr;
    QRhiTexture* m_volTex = nullptr; QRhiSampler* m_sampler = nullptr;
    QRhiShaderResourceBindings *m_lineSrb = nullptr, *m_pointSrb = nullptr, *m_sliceSrb = nullptr;
    QRhiGraphicsPipeline *m_linePs = nullptr, *m_pointPs = nullptr, *m_slicePs = nullptr;
    QShader m_lineVs, m_lineFs, m_pointVs, m_sliceVs, m_sliceFs;
    bool m_uploaded = false;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QStringList a = app.arguments();
    const std::string trk = (a.size() > 1) ? a[1].toStdString() : "/Users/haoziqi/Documents/Finch-Viewer/data/ORedited.trk";
    const std::string fa = (a.size() > 2) ? a[2].toStdString() : "/Users/haoziqi/Documents/Finch-Viewer/data/SUBG08_tissue_FA_aggressive.nii.gz";
    const QString shaderDir = (a.size() > 3) ? a[3] : QStringLiteral("/tmp/tract_rhi_build");
    QString shot;
    int si = a.indexOf("--screenshot");
    if (si >= 0 && si + 1 < a.size()) shot = a[si + 1];

    TractViewerRhi w(trk, fa, shaderDir);
    w.resize(1000, 800);
    w.setWindowTitle("Tractography — Qt RHI / Metal");
    w.show();

    if (!shot.isEmpty()) {
        QTimer::singleShot(700, [&] {
            const QImage img = w.grabFramebuffer();
            const bool ok = !img.isNull() && img.save(shot);
            std::printf("%s -> %s (%dx%d)\n", ok ? "SCREENSHOT SAVED" : "SCREENSHOT FAILED", qPrintable(shot), img.width(), img.height());
            app.quit();
        });
    }
    return app.exec();
}
