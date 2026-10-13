// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_path.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <cstdio>

#include "shell_model.h"

namespace sonora::ui {

namespace {

QString api_name(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
        case QSGRendererInterface::Software: return QStringLiteral("software scene graph");
        case QSGRendererInterface::OpenGL: return QStringLiteral("OpenGL");
        case QSGRendererInterface::Vulkan: return QStringLiteral("Vulkan");
        case QSGRendererInterface::Null: return QStringLiteral("null");
        default: return QStringLiteral("other (%1)").arg(static_cast<int>(api));
    }
}

}  // namespace

bool is_software_rasterizer(const QString& name) {
    static const char* const kNames[] = {"llvmpipe", "softpipe", "lavapipe", "swrast",
                                         "software rasterizer", "swiftshader",
                                         "microsoft basic render"};
    const QString n = name.toLower();
    for (const char* k : kNames) {
        if (n.contains(QLatin1String(k))) return true;
    }
    return false;
}

QString RenderPath::describe() const {
    QString s = api;
    if (!device.isEmpty()) s += QStringLiteral(" on ") + device;
    if (software) s += QStringLiteral(" (SOFTWARE RASTERISER)");
    return s;
}

void probe_render_path(QQuickWindow* window, ShellModel* model,
                       void (*on_known)(const RenderPath&, void*), void* ctx) {
    // sceneGraphInitialized is emitted on the render thread when there is
    // one; the RHI may only be asked about there. The result goes back by a
    // queued call, so the model is only ever touched on the GUI thread.
    QObject::connect(
        window, &QQuickWindow::sceneGraphInitialized, window,
        [window, model, on_known, ctx]() {
            RenderPath p;
            QSGRendererInterface* ri = window->rendererInterface();
            const auto api = ri->graphicsApi();
            p.api = api_name(api);
            if (api == QSGRendererInterface::Software) {
                p.software = true;
                p.meters_available = false;
                p.device = QStringLiteral("QPainter");
            } else if (api == QSGRendererInterface::OpenGL) {
                // QRhi's driverInfo() would be the API-neutral way, but its
                // header ships only with Qt's private development package.
                // The GL renderer string is what Mesa names llvmpipe by.
                if (QOpenGLContext* gl = QOpenGLContext::currentContext()) {
                    const auto* r = reinterpret_cast<const char*>(
                        gl->functions()->glGetString(GL_RENDERER));
                    p.device = r ? QString::fromLatin1(r) : QStringLiteral("unknown GL renderer");
                }
                p.software = is_software_rasterizer(p.device);
            } else {
                // Not OpenGL, and nothing here can name the device. Unknown
                // is said as unknown; it is not counted as a GPU.
                p.device = QStringLiteral("device not identified");
            }
            std::fprintf(stderr, "sonora-ui: rendering path: %s\n",
                         qPrintable(p.describe()));
            if (p.software) {
                std::fprintf(stderr,
                             "sonora-ui: WARNING: a CPU rasteriser is drawing this "
                             "interface. ADR-0006 requires a working GPU driver; this is "
                             "not a supported configuration.\n");
            }
            QMetaObject::invokeMethod(
                model,
                [model, p, on_known, ctx]() {
                    model->setRenderPath(p.describe(), p.software, p.meters_available);
                    if (on_known) on_known(p, ctx);
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
}

}  // namespace sonora::ui
