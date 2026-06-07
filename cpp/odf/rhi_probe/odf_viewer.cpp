// Interactive ODF glyph viewer: a QRhiWidget (Metal) running the fused GPU pipeline
// live — compute deform + programmable-vertex-pulling instanced draw — with an orbit
// camera (left-drag rotate, wheel zoom). The compute pass runs once (geometry is
// static); only the camera UBO updates per frame.
//
// Run:        odf_viewer <odf.nii.gz> <shaderDir>
// Verify:     QT_QPA_PLATFORM=offscreen odf_viewer <odf.nii.gz> <shaderDir> --screenshot out.png

#include "odf_volume.hpp"
#include "sh_basis.hpp"
#include "icosphere.hpp"
#include "odf_types.hpp"

#include <rhi/qrhi.h>
#include <QRhiWidget>
#include <QApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
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

class OdfGlyphWidget : public QRhiWidget {
public:
    OdfGlyphWidget(const std::string& niiPath, const QString& shaderDir)
        : m_shaderDir(shaderDir) {
        setApi(QRhiWidget::Api::Metal);
        setSampleCount(4);
        loadData(niiPath);
    }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (m_rhi != rhi()) { releaseAll(); m_rhi = rhi(); }
        if (!m_gPipe) createResources();
    }

    void render(QRhiCommandBuffer* cb) override {
        QRhiRenderTarget* rt = renderTarget();
        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();

        if (!m_uploaded) {
            u->uploadStaticBuffer(m_bCoeff, m_coeffs.data());
            u->uploadStaticBuffer(m_bBasis, m_basis.data());
            u->uploadStaticBuffer(m_bDirs, m_dirs4.data());
            u->uploadStaticBuffer(m_bIdxStor, m_idx.data());
            u->uploadStaticBuffer(m_bCenters, m_centers4.data());
            u->uploadStaticBuffer(m_bIdxDraw, m_idx.data());
            struct CUbo { int nVox, nDir, nTri, nCoeffs; float scale; int norm; int clampN; float eps; }
                cu{ m_nVox, m_nDir, m_nTri, m_nC, m_scale, 1, 1, 1e-4f };
            u->updateDynamicBuffer(m_bCUbo, 0, sizeof(cu), &cu);
            m_uploaded = true;
        }

        // camera UBO every frame
        const QMatrix4x4 mvp = computeMvp(rt->pixelSize());
        struct RUbo { float mvp[16]; float light[4]; int nDir; int pad[3]; } ru{};
        std::memcpy(ru.mvp, mvp.constData(), 16 * sizeof(float));
        const QVector3D L = QVector3D(0.4f, -0.7f, 0.6f).normalized();
        ru.light[0] = L.x(); ru.light[1] = L.y(); ru.light[2] = L.z();
        ru.nDir = m_nDir;
        u->updateDynamicBuffer(m_bRUbo, 0, sizeof(ru), &ru);

        if (!m_computeDone) {
            cb->beginComputePass(u);
            cb->setComputePipeline(m_cPipe);
            cb->setShaderResources(m_cSrb);
            cb->dispatch((m_nVox + 63) / 64, 1, 1);
            cb->endComputePass();
            m_computeDone = true;
            cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), { 1.0f, 0 }, nullptr);
        } else {
            cb->beginPass(rt, QColor::fromRgbF(0.035, 0.035, 0.04), { 1.0f, 0 }, u);
        }

        const QSize px = rt->pixelSize();
        cb->setGraphicsPipeline(m_gPipe);
        cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
        cb->setShaderResources(m_gSrb);
        cb->setVertexInput(0, 0, nullptr, m_bIdxDraw, 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(quint32(m_idx.size()), quint32(m_nVox));
        cb->endPass();
    }

    void releaseResources() override { releaseAll(); }

    void mousePressEvent(QMouseEvent* e) override { m_lastPos = e->position().toPoint(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) {
            const QPoint p = e->position().toPoint();
            m_yaw   += (p.x() - m_lastPos.x()) * 0.01f;
            m_pitch += (p.y() - m_lastPos.y()) * 0.01f;
            m_pitch = std::clamp(m_pitch, -1.4f, 1.4f);
            m_lastPos = p;
            update();
        }
    }
    void wheelEvent(QWheelEvent* e) override {
        m_dist *= std::pow(0.9f, e->angleDelta().y() / 120.0f);
        m_dist = std::clamp(m_dist, 0.05f * m_diag, 50.0f * m_diag);
        update();
    }

private:
    void loadData(const std::string& niiPath) {
        OdfVolume vol = LoadOdfNifti(niiPath);
        const ShOrder order = InferShOrder(std::size_t(vol.nCoeffs));
        const Icosphere ico = MakeIcosphere(3);
        const ShBasisMatrix B = BuildShBasisMatrix(ico.vertices, order);
        m_nDir = int(ico.vertexCount());
        m_nC = vol.nCoeffs;
        m_nTri = int(ico.indices.size() / 3);
        m_basis = B.values;
        m_idx = ico.indices;

        auto colLen = [&](int c) {
            return std::sqrt(vol.affine.at(0, c) * vol.affine.at(0, c) +
                             vol.affine.at(1, c) * vol.affine.at(1, c) +
                             vol.affine.at(2, c) * vol.affine.at(2, c));
        };
        const float voxSize = std::min({ colLen(0), colLen(1), colLen(2) });
        m_scale = 0.45f * voxSize;

        std::vector<VoxelIndex> vox;
        for (int k = 0; k < vol.dims[2]; ++k)
            for (int j = 0; j < vol.dims[1]; ++j)
                for (int i = 0; i < vol.dims[0]; ++i) vox.push_back({ i, j, k });
        m_nVox = int(vox.size());

        m_coeffs.resize(std::size_t(m_nVox) * m_nC);
        for (int g = 0; g < m_nVox; ++g)
            for (int c = 0; c < m_nC; ++c)
                m_coeffs[std::size_t(g) * m_nC + c] = vol.coeffs[vol.coeffIndex(vox[g].i, vox[g].j, vox[g].k, c)];

        m_dirs4.assign(std::size_t(m_nDir) * 4, 0.0f);
        for (int v = 0; v < m_nDir; ++v) {
            m_dirs4[std::size_t(v) * 4 + 0] = ico.vertices[v].x;
            m_dirs4[std::size_t(v) * 4 + 1] = ico.vertices[v].y;
            m_dirs4[std::size_t(v) * 4 + 2] = ico.vertices[v].z;
        }
        m_centers4.assign(std::size_t(m_nVox) * 4, 0.0f);
        for (int g = 0; g < m_nVox; ++g) {
            const Vec3 w = VoxelToWorld(vol.affine, vox[g]);
            m_centers4[std::size_t(g) * 4 + 0] = w.x;
            m_centers4[std::size_t(g) * 4 + 1] = w.y;
            m_centers4[std::size_t(g) * 4 + 2] = w.z;
        }

        m_center = qv(vol.worldBounds.center());
        const Vec3 ext = vol.worldBounds.extent();
        m_diag = std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z) + 2.0f * m_scale;
        m_dist = 1.8f * m_diag;
    }

    QMatrix4x4 computeMvp(const QSize& px) const {
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        const QVector3D offset = m_dist * QVector3D(cp * std::cos(m_yaw), cp * std::sin(m_yaw), sp);
        const QVector3D eye = m_center + offset;
        QMatrix4x4 proj; proj.perspective(40.0f, px.height() ? float(px.width()) / float(px.height()) : 1.0f,
                                          0.01f * m_diag, 100.0f * m_diag);
        QMatrix4x4 view; view.lookAt(eye, m_center, QVector3D(0, 0, 1));
        return m_rhi->clipSpaceCorrMatrix() * proj * view;
    }

    void createResources() {
        m_cs = loadShader(m_shaderDir + "/odf_deform_full.comp.qsb");
        m_vs = loadShader(m_shaderDir + "/glyph_pull.vert.qsb");
        m_fs = loadShader(m_shaderDir + "/glyph.frag.qsb");

        auto mkBuf = [&](QRhiBuffer::Type t, QRhiBuffer::UsageFlags us, quint32 sz) {
            QRhiBuffer* b = m_rhi->newBuffer(t, us, sz); b->create(); return b;
        };
        const quint32 posBytes = quint32(std::size_t(m_nVox) * m_nDir * 4 * sizeof(float));
        m_bCoeff   = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(m_coeffs.size() * sizeof(float)));
        m_bBasis   = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(m_basis.size() * sizeof(float)));
        m_bDirs    = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(m_dirs4.size() * sizeof(float)));
        m_bIdxStor = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(m_idx.size() * sizeof(unsigned)));
        m_bCenters = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(m_centers4.size() * sizeof(float)));
        m_bRadii   = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, quint32(std::size_t(m_nVox) * m_nDir * sizeof(float)));
        m_bPos     = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, posBytes);
        m_bNor     = mkBuf(QRhiBuffer::Static,    QRhiBuffer::StorageBuffer, posBytes);
        m_bCUbo    = mkBuf(QRhiBuffer::Dynamic,   QRhiBuffer::UniformBuffer, 256);
        m_bRUbo    = mkBuf(QRhiBuffer::Dynamic,   QRhiBuffer::UniformBuffer, 256);
        m_bIdxDraw = mkBuf(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,   quint32(m_idx.size() * sizeof(unsigned)));

        using SRB = QRhiShaderResourceBinding;
        m_cSrb = m_rhi->newShaderResourceBindings();
        m_cSrb->setBindings({
            SRB::bufferLoad     (0, SRB::ComputeStage, m_bCoeff),
            SRB::bufferLoad     (1, SRB::ComputeStage, m_bBasis),
            SRB::bufferLoad     (2, SRB::ComputeStage, m_bDirs),
            SRB::bufferLoad     (3, SRB::ComputeStage, m_bIdxStor),
            SRB::bufferLoad     (4, SRB::ComputeStage, m_bCenters),
            SRB::bufferLoadStore(5, SRB::ComputeStage, m_bRadii),
            SRB::bufferLoadStore(6, SRB::ComputeStage, m_bPos),
            SRB::bufferLoadStore(7, SRB::ComputeStage, m_bNor),
            SRB::uniformBuffer  (8, SRB::ComputeStage, m_bCUbo),
        });
        m_cSrb->create();

        m_gSrb = m_rhi->newShaderResourceBindings();
        m_gSrb->setBindings({
            SRB::uniformBuffer(0, SRB::VertexStage | SRB::FragmentStage, m_bRUbo),
            SRB::bufferLoad   (1, SRB::VertexStage, m_bPos),
            SRB::bufferLoad   (2, SRB::VertexStage, m_bNor),
            SRB::bufferLoad   (3, SRB::VertexStage, m_bDirs),
        });
        m_gSrb->create();

        m_cPipe = m_rhi->newComputePipeline();
        m_cPipe->setShaderStage({ QRhiShaderStage::Compute, m_cs });
        m_cPipe->setShaderResourceBindings(m_cSrb);
        m_cPipe->create();

        m_gPipe = m_rhi->newGraphicsPipeline();
        m_gPipe->setShaderStages({ { QRhiShaderStage::Vertex, m_vs }, { QRhiShaderStage::Fragment, m_fs } });
        QRhiVertexInputLayout emptyLayout;
        m_gPipe->setVertexInputLayout(emptyLayout);
        m_gPipe->setShaderResourceBindings(m_gSrb);
        m_gPipe->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_gPipe->setSampleCount(renderTarget()->sampleCount());
        m_gPipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_gPipe->setCullMode(QRhiGraphicsPipeline::None);
        m_gPipe->setDepthTest(true);
        m_gPipe->setDepthWrite(true);
        m_gPipe->setDepthOp(QRhiGraphicsPipeline::Less);
        m_gPipe->create();

        m_uploaded = false;
        m_computeDone = false;
    }

    void releaseAll() {
        for (QRhiResource** r : { (QRhiResource**)&m_gPipe, (QRhiResource**)&m_cPipe,
                                  (QRhiResource**)&m_gSrb, (QRhiResource**)&m_cSrb,
                                  (QRhiResource**)&m_bIdxDraw, (QRhiResource**)&m_bRUbo, (QRhiResource**)&m_bCUbo,
                                  (QRhiResource**)&m_bNor, (QRhiResource**)&m_bPos, (QRhiResource**)&m_bRadii,
                                  (QRhiResource**)&m_bCenters, (QRhiResource**)&m_bIdxStor, (QRhiResource**)&m_bDirs,
                                  (QRhiResource**)&m_bBasis, (QRhiResource**)&m_bCoeff }) {
            if (*r) { (*r)->destroy(); delete *r; *r = nullptr; }
        }
    }

    // CPU-prepared inputs
    std::vector<float> m_coeffs, m_basis, m_dirs4, m_centers4;
    std::vector<unsigned> m_idx;
    int m_nVox = 0, m_nDir = 0, m_nTri = 0, m_nC = 0;
    float m_scale = 1.0f;
    QString m_shaderDir;

    // camera
    QVector3D m_center;
    float m_diag = 1.0f, m_yaw = 0.7f, m_pitch = 0.5f, m_dist = 1.0f;
    QPoint m_lastPos;

    // rhi
    QRhi* m_rhi = nullptr;
    QRhiBuffer *m_bCoeff = nullptr, *m_bBasis = nullptr, *m_bDirs = nullptr, *m_bIdxStor = nullptr,
               *m_bCenters = nullptr, *m_bRadii = nullptr, *m_bPos = nullptr, *m_bNor = nullptr,
               *m_bCUbo = nullptr, *m_bRUbo = nullptr, *m_bIdxDraw = nullptr;
    QRhiComputePipeline* m_cPipe = nullptr;
    QRhiGraphicsPipeline* m_gPipe = nullptr;
    QRhiShaderResourceBindings* m_cSrb = nullptr;
    QRhiShaderResourceBindings* m_gSrb = nullptr;
    QShader m_cs, m_vs, m_fs;
    bool m_uploaded = false, m_computeDone = false;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QStringList a = app.arguments();
    const std::string nii = (a.size() > 1) ? a[1].toStdString()
                                           : "/Users/haoziqi/Documents/Finch-Viewer/dmri-explorer/data/odf.nii.gz";
    const QString shaderDir = (a.size() > 2) ? a[2] : QStringLiteral("/tmp/odf_rhi_build");
    QString shot;
    int si = a.indexOf("--screenshot");
    if (si >= 0 && si + 1 < a.size()) shot = a[si + 1];

    OdfGlyphWidget w(nii, shaderDir);
    w.resize(900, 700);
    w.setWindowTitle("ODF glyphs — Qt RHI / Metal");
    w.show();

    if (!shot.isEmpty()) {
        QTimer::singleShot(600, [&] {
            const QImage img = w.grabFramebuffer();
            const bool ok = !img.isNull() && img.save(shot);
            std::printf("%s -> %s (%dx%d)\n", ok ? "SCREENSHOT SAVED" : "SCREENSHOT FAILED",
                        qPrintable(shot), img.width(), img.height());
            app.quit();
        });
    }
    return app.exec();
}
