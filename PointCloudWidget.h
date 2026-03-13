#ifndef POINTCLOUDWIDGET_H
#define POINTCLOUDWIDGET_H

#include <QOpenGLWidget>
#ifdef Q_OS_ANDROID
    #include <QOpenGLExtraFunctions>
#else
    #include <QOpenGLFunctions_3_3_Core>
#endif
#include <QMatrix4x4>
#include <QVector3D>
#include <vector>
#include <QRegularExpression>
#include <chrono>
#include <deque>

class QOpenGLShaderProgram;

// Camera class for orbit-style interaction
class Camera
{
public:
    Camera();

    // Camera control
    void rotate(float deltaAzimuth, float deltaElevation);
    void zoom(float deltaDistance);
    void pan(float deltaX, float deltaY);
    void setTarget(const QVector3D &target);
    void setDistance(float distance);

    // --- FIX: expose distance so gesture handler can scale pan/zoom ---
    float distance() const { return m_distance; }

    // Get matrices
    QMatrix4x4 getViewMatrix() const;
    QMatrix4x4 getProjectionMatrix(float aspectRatio) const;

    // Reset camera
    void reset();

private:
    void updateViewMatrix();

    // Spherical coordinates (orbit camera)
    float m_distance;      // Distance from target
    float m_azimuth;       // Rotation around Z axis (in degrees)
    float m_elevation;     // Elevation angle (in degrees)
    QVector3D m_target;    // Look-at point
    QVector3D m_up;        // Up vector

    // Camera settings
    float m_fov;           // Field of view
    float m_nearPlane;
    float m_farPlane;

    // Cached view matrix
    mutable QMatrix4x4 m_viewMatrix;
    mutable bool m_viewMatrixDirty;
};

// Structure to hold point data with timestamp
struct PointCloudFrame
{
    std::vector<float> positions;  // x, y, z interleaved
    std::chrono::steady_clock::time_point timestamp;
    
    PointCloudFrame() : timestamp(std::chrono::steady_clock::now()) {}
    PointCloudFrame(const std::vector<float>& pos) 
        : positions(pos), timestamp(std::chrono::steady_clock::now()) {}
};

// Renderer class that handles all OpenGL operations with decay support
class PointCloudRenderer
{
public:
    PointCloudRenderer();
    ~PointCloudRenderer();

    // Initialize OpenGL resources
#ifdef Q_OS_ANDROID
    bool initialize(QOpenGLExtraFunctions *gl);
#else
    bool initialize(QOpenGLFunctions_3_3_Core *gl);
#endif

    // Generate dummy point cloud (fewer points on Android)
    void generateDummyPointCloud(size_t numPoints);

    // Update with new streaming point cloud data
    void updatePointCloud(const std::vector<float> &points);
    
    // Load static point cloud (replaces accumulated data)
    void loadStaticPointCloud(const std::vector<float> &points);
    
    // Clear all accumulated points
    void clearAccumulatedPoints();
    
    // Decay settings
    void setDecayEnabled(bool enabled) { m_decayEnabled = enabled; }
    void setDecayTime(float seconds) { m_decayTime = seconds; }
    void setMaxPoints(size_t maxPoints) { m_maxPoints = maxPoints; }
    
    bool isDecayEnabled() const { return m_decayEnabled; }
    float getDecayTime() const { return m_decayTime; }
    size_t getMaxPoints() const { return m_maxPoints; }

    // Render the point cloud
#ifdef Q_OS_ANDROID
    void render(QOpenGLExtraFunctions *gl, 
                const QMatrix4x4 &viewMatrix, 
                const QMatrix4x4 &projectionMatrix);
#else
    void render(QOpenGLFunctions_3_3_Core *gl, 
                const QMatrix4x4 &viewMatrix, 
                const QMatrix4x4 &projectionMatrix);
#endif

    // Render coordinate axes at origin
#ifdef Q_OS_ANDROID
    void renderAxes(QOpenGLExtraFunctions *gl,
                    const QMatrix4x4 &viewMatrix,
                    const QMatrix4x4 &projectionMatrix,
                    float axisLength = 1.0f);
#else
    void renderAxes(QOpenGLFunctions_3_3_Core *gl,
                    const QMatrix4x4 &viewMatrix,
                    const QMatrix4x4 &projectionMatrix,
                    float axisLength = 1.0f);
#endif

    // Render path (trajectory)
#ifdef Q_OS_ANDROID
    void renderPath(QOpenGLExtraFunctions *gl,
                    const QMatrix4x4 &viewMatrix,
                    const QMatrix4x4 &projectionMatrix,
                    const std::vector<QVector3D> &path);
#else
    void renderPath(QOpenGLFunctions_3_3_Core *gl,
                    const QMatrix4x4 &viewMatrix,
                    const QMatrix4x4 &projectionMatrix,
                    const std::vector<QVector3D> &path);
#endif

    // Render robot frame (axes at robot position/orientation)
#ifdef Q_OS_ANDROID
    void renderRobotFrame(QOpenGLExtraFunctions *gl,
                          const QMatrix4x4 &viewMatrix,
                          const QMatrix4x4 &projectionMatrix,
                          const QVector3D &position,
                          const QQuaternion &orientation,
                          float axisLength = 0.5f);
#else
    void renderRobotFrame(QOpenGLFunctions_3_3_Core *gl,
                          const QMatrix4x4 &viewMatrix,
                          const QMatrix4x4 &projectionMatrix,
                          const QVector3D &position,
                          const QQuaternion &orientation,
                          float axisLength = 0.5f);
#endif

    // Cleanup
#ifdef Q_OS_ANDROID
    void cleanup(QOpenGLExtraFunctions *gl);
#else
    void cleanup(QOpenGLFunctions_3_3_Core *gl);
#endif

    // Get point count
    size_t getPointCount() const { return m_totalPoints; }
    size_t getFrameCount() const { return m_pointFrames.size(); }

private:
#ifdef Q_OS_ANDROID
    bool createShaders(QOpenGLExtraFunctions *gl);
    bool createBuffers(QOpenGLExtraFunctions *gl);
    void updateVBO(QOpenGLExtraFunctions *gl);
    bool createAxisShaders(QOpenGLExtraFunctions *gl);
    bool createAxisBuffers(QOpenGLExtraFunctions *gl);
#else
    bool createShaders(QOpenGLFunctions_3_3_Core *gl);
    bool createBuffers(QOpenGLFunctions_3_3_Core *gl);
    void updateVBO(QOpenGLFunctions_3_3_Core *gl);
    bool createAxisShaders(QOpenGLFunctions_3_3_Core *gl);
    bool createAxisBuffers(QOpenGLFunctions_3_3_Core *gl);
#endif

    void applyDecay();
    void rebuildBufferData();

    // OpenGL resources
    GLuint m_vao;
    GLuint m_vbo;
    GLuint m_shaderProgram;
    
    // Axis rendering resources
    GLuint m_axisVao;
    GLuint m_axisVbo;
    GLuint m_axisShaderProgram;
    GLint m_axisMvpLocation;
    GLint m_axisColorLocation;
    
    // Path rendering resources
    GLuint m_pathVao;
    GLuint m_pathVbo;
    size_t m_pathPointCount;
    
    // Shader locations
    GLint m_mvpLocation;
    GLint m_positionLocation;
    GLint m_intensityLocation;

    // Accumulated point cloud data with timestamps
    std::deque<PointCloudFrame> m_pointFrames;
    
    // Flattened buffer data for GPU (position + intensity)
    std::vector<float> m_bufferData; // x, y, z, intensity per point
    size_t m_totalPoints;

    // Decay settings
    bool m_decayEnabled;
    float m_decayTime;  // Time in seconds for points to completely fade
    size_t m_maxPoints; // Maximum number of points to keep

    // Optimization flags
    bool m_bufferNeedsUpdate;  // Flag to indicate buffer needs rebuilding
    int m_framesSinceLastDecay; // Counter to reduce decay calculation frequency

    // Initialization flag
    bool m_initialized;
};

// Main widget class
#ifdef Q_OS_ANDROID
class PointCloudWidget : public QOpenGLWidget, protected QOpenGLExtraFunctions
#else
class PointCloudWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
#endif
{
    Q_OBJECT

public:
    explicit PointCloudWidget(QWidget *parent = nullptr);
    ~PointCloudWidget() override;
    
    // Load point cloud from file (static)
    bool loadPointCloud(const QString &filename);
    
    // Update path from ROS
    void updatePath(const std::vector<QVector3D> &path);
    
    // Update robot pose from ROS
    void updateRobotPose(const QVector3D &position, const QQuaternion &orientation);
    
    PointCloudRenderer m_renderer;
    
    // Get bounding box info
    QVector3D getBoundingBoxMin() const { return m_boundingBoxMin; }
    QVector3D getBoundingBoxMax() const { return m_boundingBoxMax; }

protected:
    // QOpenGLWidget interface
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // Mouse interaction (Windows / desktop)
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

#ifdef Q_OS_ANDROID
    // Touch event handling for Android
    bool event(QEvent *event) override;
#endif

private:
    Camera m_camera;
    
    // Bounding box
    QVector3D m_boundingBoxMin;
    QVector3D m_boundingBoxMax;
    
    // Path and robot pose data
    std::vector<QVector3D> m_path;
    QVector3D m_robotPosition;
    QQuaternion m_robotOrientation;
    bool m_hasRobotPose;

    // Mouse interaction state (desktop)
    QPoint m_lastMousePos;
    bool m_leftButtonPressed;
    bool m_rightButtonPressed;

#ifdef Q_OS_ANDROID
    // -----------------------------------------------------------------------
    // FIX: expanded touch state — pan midpoint + rotation angle added
    // -----------------------------------------------------------------------
    QPointF m_lastTouchPos;          // single-finger last position
    qreal   m_lastPinchDistance;     // two-finger last distance (logical px)
    QPointF m_lastPinchMidpoint;     // two-finger last midpoint
    float   m_lastPinchAngle;        // two-finger last angle (degrees)
    bool    m_isPinching;            // true once a 2-finger gesture has started
    int     m_lastActiveFingerCount; // FIX: track previous frame finger count to detect transitions
#endif
};

#endif // POINTCLOUDWIDGET_H
