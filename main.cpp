#include "MainWindow.h"
#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

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
    window.resize(1024, 768);
    window.show();

    return app.exec();
}
