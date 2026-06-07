// Headless probe: does Qt RHI -> Metal compute actually work on THIS machine?
//
// This is a de-risking smoke test for the renderer upgrade (QOpenGLWidget -> Qt RHI):
// macOS system OpenGL is frozen at 4.1 (no compute), so the ODF GPU path must run on
// Metal via Qt RHI. Before investing in the real ODF compute pipeline we verify the
// whole chain here: create a Metal QRhi, query the Compute feature + threadgroup
// limits, build a compute pipeline from a qsb-baked shader, dispatch it offscreen,
// read the storage buffer back, and check the numbers. No window, no GL.
//
// Build: standalone CMake in this dir -> /tmp (see CMakeLists.txt). NOT wired into the
// repo's root CMakeLists, so it never collides with the OpenGL editor build.

#include <rhi/qrhi.h>
#include <QGuiApplication>
#include <QFile>
#include <QByteArray>
#include <cmath>
#include <cstdio>

static QShader loadShader(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");  // no window server needed
    QGuiApplication app(argc, argv);

    const QString qsbPath = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral("deform_probe.comp.qsb");

    QRhiMetalInitParams params;
    QRhi *rhi = QRhi::create(QRhi::Metal, &params);
    if (!rhi) { std::fprintf(stderr, "FAIL: QRhi::create(Metal) returned null\n"); return 2; }

    const QRhiDriverInfo info = rhi->driverInfo();
    std::printf("backend      : %s\n", rhi->backendName());
    std::printf("device       : %s\n", info.deviceName.constData());
    std::printf("deviceType   : %d (0=unknown 1=integrated 2=discrete)\n", int(info.deviceType));

    const bool compute = rhi->isFeatureSupported(QRhi::Compute);
    std::printf("Compute      : %s\n", compute ? "SUPPORTED" : "NOT supported");
    std::printf("MaxThreadsPerThreadGroup : %d\n", rhi->resourceLimit(QRhi::MaxThreadsPerThreadGroup));
    std::printf("MaxThreadGroupX/Y/Z      : %d / %d / %d\n",
                rhi->resourceLimit(QRhi::MaxThreadGroupX),
                rhi->resourceLimit(QRhi::MaxThreadGroupY),
                rhi->resourceLimit(QRhi::MaxThreadGroupZ));

    if (!compute) { delete rhi; return 3; }

    QShader cs = loadShader(qsbPath);
    if (!cs.isValid()) { std::fprintf(stderr, "FAIL: cannot load/parse %s\n", qPrintable(qsbPath)); delete rhi; return 4; }
    std::printf("shader       : loaded %s (valid)\n", qPrintable(qsbPath));

    const int N = 64;
    QRhiBuffer *outBuf = rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, N * sizeof(float));
    outBuf->create();
    QRhiBuffer *ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    ubuf->create();

    QRhiShaderResourceBindings *srb = rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::bufferStore(0, QRhiShaderResourceBinding::ComputeStage, outBuf),
        QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::ComputeStage, ubuf),
    });
    srb->create();

    QRhiComputePipeline *pipe = rhi->newComputePipeline();
    pipe->setShaderStage({ QRhiShaderStage::Compute, cs });
    pipe->setShaderResourceBindings(srb);
    if (!pipe->create()) { std::fprintf(stderr, "FAIL: compute pipeline create()\n"); delete rhi; return 5; }
    std::printf("pipeline     : compute pipeline created\n");

    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        std::fprintf(stderr, "FAIL: beginOffscreenFrame\n"); delete rhi; return 6;
    }

    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    int n = N;
    u->updateDynamicBuffer(ubuf, 0, sizeof(int), &n);
    cb->beginComputePass(u);
    cb->setComputePipeline(pipe);
    cb->setShaderResources(srb);
    cb->dispatch(1, 1, 1);  // local_size_x=64 -> one group covers N=64
    cb->endComputePass();

    QRhiReadbackResult rb;
    QRhiResourceUpdateBatch *u2 = rhi->nextResourceUpdateBatch();
    u2->readBackBuffer(outBuf, 0, N * sizeof(float), &rb);
    cb->resourceUpdate(u2);

    rhi->endOffscreenFrame();  // blocks until GPU done -> rb.data is ready

    bool ok = (rb.data.size() == int(N * sizeof(float)));
    if (ok) {
        const float *vals = reinterpret_cast<const float *>(rb.data.constData());
        for (int i = 0; i < N; ++i) {
            if (std::fabs(vals[i] - float(i) * 2.0f) > 1e-4f) {
                ok = false;
                std::fprintf(stderr, "mismatch at %d: got %f, want %f\n", i, vals[i], float(i) * 2.0f);
                break;
            }
        }
        if (ok)
            std::printf("readback     : vals[10]=%.1f (want 20)  vals[63]=%.1f (want 126)\n",
                        double(vals[10]), double(vals[63]));
    }

    std::printf("\n%s\n", ok ? "PROBE PASSED: Metal compute executes and readback is correct."
                             : "PROBE FAILED: compute readback mismatch.");

    delete pipe; delete srb; delete ubuf; delete outBuf; delete rhi;
    return ok ? 0 : 7;
}
