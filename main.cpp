#include "MainWindow.h"
#include "PointCloudWidget.h"
#include <QApplication>
#include <QSurfaceFormat>

#ifdef Q_OS_ANDROID
#include <QTouchEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include <QJniEnvironment>
#include <jni.h>

// libgstreamer_android.so (built via GStreamer's own ndk-build tooling —
// see CMakeLists.txt's GStreamer section) exports a standard JNI_OnLoad,
// same as any other native library the JVM loads. Android's dynamic
// linker pulls that .so in automatically as a dependency of our own
// native library, but only the library actually passed to
// System.loadLibrary() gets its JNI_OnLoad invoked automatically — a
// transitively-linked dependency does not. So this app has to call it
// itself, once, before anything touches GStreamer (RtspVideoClient's
// constructor is the first caller via gst_init(), which needs the plugin
// registry JNI_OnLoad sets up — androidmedia's MediaCodec access in
// particular needs the JavaVM it records here). Confirmed via `nm` against
// the actual downloaded SDK — earlier attempts at a function literally
// named gst_android_init() were guessing at a name from an older
// tutorial/SDK layout that doesn't match this version.
extern "C" jint JNI_OnLoad(JavaVM *vm, void *reserved);
#endif // Q_OS_ANDROID

#ifdef Q_OS_ANDROID
// ============================================================================
// FIX: buttons / sidebar dead after disabling touch->mouse synthesis
//
// Qt::AA_SynthesizeMouseForUnhandledTouchEvents=false (see below) is
// necessary to fix PointCloudWidget's 2-finger gesture race, but it is an
// application-wide switch: with it off, ANY plain widget that never opts
// into raw touch events itself — QPushButton, the custom SidebarButton
// (a plain QWidget with its own mouseReleaseEvent-based click detection),
// or anything else built the normal Qt way — receives neither a
// synthesized mouse event nor a QTouchEvent. Its input is simply dropped.
// That's why the mini player buttons AND the sidebar (SidebarButton isn't
// a QAbstractButton, so a button-specific fix doesn't reach it) both went
// dead.
//
// Rather than special-casing each widget type, this restores Qt's normal
// default behavior everywhere EXCEPT PointCloudWidget: every other widget
// gets touch enabled (via the one-time Polish lifecycle event, so this
// covers widgets created later too, e.g. inside the settings dialog, with
// no per-widget wiring needed), and a tap is relayed as a real
// press-then-release QMouseEvent to that exact widget — the same event
// pair Qt's own synthesis would have produced, so every widget's existing
// mouse-based click logic (QAbstractButton's built-in state machine,
// SidebarButton's custom mouseReleaseEvent, or anything else) just works,
// unchanged.
// ============================================================================
class TouchToMouseRelay : public QObject
{
public:
    explicit TouchToMouseRelay(QObject *parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || isPointCloudWidgetOrChild(widget)) {
            return false; // PointCloudWidget manages its own touch entirely
        }

        // One-time hook, right before a widget is first shown: opt every
        // other widget into raw touch so Qt actually delivers touch events
        // to it instead of silently dropping them.
        if (event->type() == QEvent::Polish) {
            widget->setAttribute(Qt::WA_AcceptTouchEvents, true);
            return false;
        }

        switch (event->type()) {
            case QEvent::TouchBegin: {
                auto *touchEvent = static_cast<QTouchEvent *>(event);
                if (touchEvent->points().isEmpty()) return false;
                touchEvent->accept();

                m_pressedWidget = widget;
                const QPoint localPos = touchEvent->points().first().position().toPoint();
                QMouseEvent press(QEvent::MouseButtonPress, localPos,
                                   widget->mapToGlobal(localPos),
                                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(widget, &press);
                return true;
            }
            case QEvent::TouchUpdate: {
                auto *touchEvent = static_cast<QTouchEvent *>(event);
                touchEvent->accept();
                if (m_pressedWidget == widget && !touchEvent->points().isEmpty()) {
                    const QPoint localPos = touchEvent->points().first().position().toPoint();
                    QMouseEvent move(QEvent::MouseMove, localPos,
                                      widget->mapToGlobal(localPos),
                                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(widget, &move);
                }
                return true;
            }
            case QEvent::TouchEnd: {
                auto *touchEvent = static_cast<QTouchEvent *>(event);
                touchEvent->accept();
                if (m_pressedWidget == widget && !touchEvent->points().isEmpty()) {
                    const QPoint localPos = touchEvent->points().first().position().toPoint();
                    QMouseEvent release(QEvent::MouseButtonRelease, localPos,
                                         widget->mapToGlobal(localPos),
                                         Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(widget, &release);
                }
                m_pressedWidget = nullptr;
                return true;
            }
            case QEvent::TouchCancel: {
                m_pressedWidget = nullptr;
                return true;
            }
            default:
                return false;
        }
    }

private:
    static bool isPointCloudWidgetOrChild(QWidget *w)
    {
        for (QWidget *p = w; p; p = p->parentWidget()) {
            if (qobject_cast<PointCloudWidget *>(p)) return true;
        }
        return false;
    }

    QPointer<QWidget> m_pressedWidget;
};
#endif // Q_OS_ANDROID

int main(int argc, char *argv[])
{
    // -------------------------------------------------------------------
    // FIX (root cause of the 2-finger gesture race condition):
    //
    // On Android, Qt's platform plugin synthesizes a "phantom" QMouseEvent
    // from the first touch point *in addition to* the real QTouchEvent.
    // PointCloudWidget handles real touch events itself (see
    // PointCloudWidget::event()), so this synthesized mouse event is
    // redundant — but it still reaches mousePressEvent/mouseMoveEvent and
    // keeps calling m_camera.rotate() every frame, in parallel with
    // whatever the touch handler is doing.
    //
    // The effect: as soon as a second finger touches down, the touch
    // handler correctly switches to 2-finger pinch/pan, but the
    // synthesized mouse-move (still following just one finger) keeps
    // fighting it with single-finger rotation every frame — so pinch/pan
    // never visibly takes effect unless both fingers land in the exact
    // same event batch (no window for the synthesized drag to start).
    //
    // Disabling this attribute stops Qt from generating synthesized mouse
    // events from touch input at all, so only the real QTouchEvent stream
    // reaches PointCloudWidget. Must be set before QApplication is
    // constructed. Confirmed working on device — do not remove. (Every
    // other widget dying as a side effect is fixed separately by
    // TouchToMouseRelay above, not by touching this attribute.)
    // -------------------------------------------------------------------
    QApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, false);

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Skyline");
    QCoreApplication::setApplicationName("SkylineDroneApp");

#ifdef Q_OS_ANDROID
    app.installEventFilter(new TouchToMouseRelay(&app));

    // Must run before anything calls gst_init() (RtspVideoClient does, on
    // first connect) — see the extern declaration above.
    JNI_OnLoad(QJniEnvironment::javaVM(), nullptr);
#endif

    // Set OpenGL format before creating any OpenGL contexts
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    
#ifdef Q_OS_ANDROID
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);  // OpenGL ES 3.0
#else
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
#endif
    
    format.setSamples(4); // Enable multisampling for smoother points
    QSurfaceFormat::setDefaultFormat(format);

    MainWindow window;
    // NOTE: don't call window.resize() here — MainWindow's constructor
    // already sizes itself off the actual screen geometry (DPI-aware).
    // A hardcoded resize(1024, 768) after that ran last and silently
    // overrode it, forcing a fixed 4:3 window unrelated to the real
    // screen/aspect ratio, which is what was pushing the bottom stats
    // bar off the visible area on device.
    window.show();

    return app.exec();
}
