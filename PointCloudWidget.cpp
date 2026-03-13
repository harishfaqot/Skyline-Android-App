#include "PointCloudWidget.h"
#include "PCDLoader.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLShaderProgram>
#include <QtMath>
#include <random>
#include <QFileInfo>
#include <QDir>

// ============================================================================
// Camera Implementation
// ============================================================================

Camera::Camera()
    : m_distance(10.0f)
    , m_azimuth(135.0f)     // Start from front-right angle (ROS convention)
    , m_elevation(35.0f)    // Look down at 35 degrees
    , m_target(0.0f, 0.0f, 0.0f)
    , m_up(0.0f, 0.0f, 1.0f)  // Z-up like ROS/RViz
    , m_fov(45.0f)
    , m_nearPlane(0.01f)    // Closer near plane for better close-up viewing
    , m_farPlane(10000.0f)  // Farther far plane for large scenes
    , m_viewMatrixDirty(true)
{
}

void Camera::rotate(float deltaAzimuth, float deltaElevation)
{
    m_azimuth += deltaAzimuth;
    m_elevation += deltaElevation;

    // Clamp elevation to avoid gimbal lock
    m_elevation = qBound(-89.0f, m_elevation, 89.0f);

    // Normalize azimuth to [0, 360)
    while (m_azimuth < 0.0f) m_azimuth += 360.0f;
    while (m_azimuth >= 360.0f) m_azimuth -= 360.0f;

    m_viewMatrixDirty = true;
}

void Camera::zoom(float deltaDistance)
{
    m_distance += deltaDistance;
    m_distance = qMax(0.1f, m_distance); // Prevent negative or zero distance
    m_viewMatrixDirty = true;
}

void Camera::pan(float deltaX, float deltaY)
{
    // Convert spherical to Cartesian for camera position (Z-up)
    float azimuthRad = qDegreesToRadians(m_azimuth);
    float elevationRad = qDegreesToRadians(m_elevation);

    // Calculate camera's right and up vectors in Z-up system
    QVector3D cameraPos(
        m_distance * qCos(elevationRad) * qCos(azimuthRad),
        m_distance * qCos(elevationRad) * qSin(azimuthRad),
        m_distance * qSin(elevationRad)
    );

    QVector3D forward = (m_target - cameraPos).normalized();
    QVector3D right = QVector3D::crossProduct(forward, m_up).normalized();
    QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    // Move target point
    m_target += right * deltaX + up * deltaY;

    m_viewMatrixDirty = true;
}

QMatrix4x4 Camera::getViewMatrix() const
{
    if (m_viewMatrixDirty) {
        // Convert spherical coordinates to Cartesian (Z-up ROS convention)
        float azimuthRad = qDegreesToRadians(m_azimuth);
        float elevationRad = qDegreesToRadians(m_elevation);

        // In ROS/RViz: X-forward, Y-left, Z-up
        // Azimuth rotates around Z-axis, elevation tilts up/down
        QVector3D cameraPos(
            m_distance * qCos(elevationRad) * qCos(azimuthRad),
            m_distance * qCos(elevationRad) * qSin(azimuthRad),
            m_distance * qSin(elevationRad)
        );

        cameraPos += m_target;

        m_viewMatrix.setToIdentity();
        m_viewMatrix.lookAt(cameraPos, m_target, m_up);
        m_viewMatrixDirty = false;
    }

    return m_viewMatrix;
}

QMatrix4x4 Camera::getProjectionMatrix(float aspectRatio) const
{
    QMatrix4x4 projection;
    projection.perspective(m_fov, aspectRatio, m_nearPlane, m_farPlane);
    return projection;
}

void Camera::reset()
{
    m_distance = 10.0f;
    m_azimuth = 135.0f;   // Front-right view (ROS convention)
    m_elevation = 35.0f;  // Look down slightly
    m_target = QVector3D(0.0f, 0.0f, 0.0f);
    m_up = QVector3D(0.0f, 0.0f, 1.0f);  // Z-up
    m_viewMatrixDirty = true;
}

void Camera::setTarget(const QVector3D &target)
{
    m_target = target;
    m_viewMatrixDirty = true;
}

void Camera::setDistance(float distance)
{
    m_distance = qMax(0.1f, distance);
    m_viewMatrixDirty = true;
}

// ============================================================================
// PointCloudRenderer Implementation
// ============================================================================

// Vertex shader with intensity attribute for decay
#ifdef Q_OS_ANDROID
static const char *vertexShaderSource =
"#version 300 es\n"
"precision highp float;\n"
"layout(location = 0) in vec3 position;\n"
"layout(location = 1) in float intensity;\n"
"uniform mat4 mvp;\n"
"out vec3 fragColor;\n"
"out float fragIntensity;\n"
"void main()\n"
"{\n"
"    gl_Position = mvp * vec4(position, 1.0);\n"
"    gl_PointSize = 6.0;\n"
"    float t = clamp((position.z + 2.0) / 8.0, 0.0, 1.0);\n"
"    vec3 c;\n"
"    if (t < 0.25) {\n"
"        c = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), t / 0.25);\n"
"    } else if (t < 0.5) {\n"
"        c = mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), (t - 0.25) / 0.25);\n"
"    } else if (t < 0.75) {\n"
"        c = mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), (t - 0.5) / 0.25);\n"
"    } else {\n"
"        c = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (t - 0.75) / 0.25);\n"
"    }\n"
"    fragColor = c;\n"
"    fragIntensity = intensity;\n"
"}\n";
#else
static const char *vertexShaderSource =
"#version 330 core\n"
"layout(location = 0) in vec3 position;\n"
"layout(location = 1) in float intensity;\n"
"uniform mat4 mvp;\n"
"out vec3 fragColor;\n"
"out float fragIntensity;\n"
"void main()\n"
"{\n"
"    gl_Position = mvp * vec4(position, 1.0);\n"
"    gl_PointSize = 2.0;\n"
"    float t = clamp((position.z + 2.0) / 8.0, 0.0, 1.0);\n"
"    vec3 c;\n"
"    if (t < 0.25) {\n"
"        c = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), t / 0.25);\n"
"    } else if (t < 0.5) {\n"
"        c = mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), (t - 0.25) / 0.25);\n"
"    } else if (t < 0.75) {\n"
"        c = mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), (t - 0.5) / 0.25);\n"
"    } else {\n"
"        c = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (t - 0.75) / 0.25);\n"
"    }\n"
"    fragColor = c;\n"
"    fragIntensity = intensity;\n"
"}\n";
#endif

// Fragment shader with intensity-based alpha
#ifdef Q_OS_ANDROID
static const char *fragmentShaderSource =
"#version 300 es\n"
"precision mediump float;\n"
"in vec3 fragColor;\n"
"in float fragIntensity;\n"
"out vec4 color;\n"
"void main()\n"
"{\n"
"    color = vec4(fragColor, fragIntensity);\n"
"}\n";
#else
static const char *fragmentShaderSource =
"#version 330 core\n"
"in vec3 fragColor;\n"
"in float fragIntensity;\n"
"out vec4 color;\n"
"void main()\n"
"{\n"
"    color = vec4(fragColor, fragIntensity);\n"
"}\n";
#endif

// Simple axis shaders (solid colors, no lighting)
#ifdef Q_OS_ANDROID
static const char *axisVertexShaderSource =
"#version 300 es\n"
"precision highp float;\n"
"layout(location = 0) in vec3 position;\n"
"uniform mat4 mvp;\n"
"void main()\n"
"{\n"
"    gl_Position = mvp * vec4(position, 1.0);\n"
"}\n";

static const char *axisFragmentShaderSource =
"#version 300 es\n"
"precision mediump float;\n"
"uniform vec3 color;\n"
"out vec4 fragColor;\n"
"void main()\n"
"{\n"
"    fragColor = vec4(color, 1.0);\n"
"}\n";
#else
static const char *axisVertexShaderSource =
"#version 330 core\n"
"layout(location = 0) in vec3 position;\n"
"uniform mat4 mvp;\n"
"void main()\n"
"{\n"
"    gl_Position = mvp * vec4(position, 1.0);\n"
"}\n";

static const char *axisFragmentShaderSource =
"#version 330 core\n"
"uniform vec3 color;\n"
"out vec4 fragColor;\n"
"void main()\n"
"{\n"
"    fragColor = vec4(color, 1.0);\n"
"}\n";
#endif

PointCloudRenderer::PointCloudRenderer()
    : m_vao(0)
    , m_vbo(0)
    , m_shaderProgram(0)
    , m_axisVao(0)
    , m_axisVbo(0)
    , m_axisShaderProgram(0)
    , m_axisMvpLocation(-1)
    , m_axisColorLocation(-1)
    , m_pathVao(0)
    , m_pathVbo(0)
    , m_pathPointCount(0)
    , m_mvpLocation(-1)
    , m_positionLocation(-1)
    , m_intensityLocation(-1)
    , m_totalPoints(0)
    , m_decayEnabled(true)
    , m_decayTime(5.0f)  // 5 seconds default decay time
    , m_maxPoints(1000000) // 1 million points max
    , m_initialized(false)
    , m_bufferNeedsUpdate(false)
    , m_framesSinceLastDecay(0)
{
}

PointCloudRenderer::~PointCloudRenderer()
{
    // Cleanup is done explicitly via cleanup() method
}

#ifdef Q_OS_ANDROID
bool PointCloudRenderer::initialize(QOpenGLExtraFunctions *gl)
#else
bool PointCloudRenderer::initialize(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    if (!gl) {
        qCritical() << "GL context is null";
        return false;
    }

    if (!createShaders(gl)) {
        qCritical() << "Failed to create shaders";
        return false;
    }

    if (!createBuffers(gl)) {
        qCritical() << "Failed to create buffers";
        return false;
    }

    if (!createAxisShaders(gl)) {
        qCritical() << "Failed to create axis shaders";
        return false;
    }

    if (!createAxisBuffers(gl)) {
        qCritical() << "Failed to create axis buffers";
        return false;
    }

    // Create path buffers (empty initially, filled when path data arrives)
    gl->glGenVertexArrays(1, &m_pathVao);
    gl->glGenBuffers(1, &m_pathVbo);

    m_initialized = true;
    qInfo() << "Renderer initialized successfully with decay support and coordinate axes";
    return true;
}

#ifdef Q_OS_ANDROID
bool PointCloudRenderer::createShaders(QOpenGLExtraFunctions *gl)
#else
bool PointCloudRenderer::createShaders(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    GLint success;
    GLchar infoLog[512];

    // Create vertex shader
    GLuint vertexShader = gl->glCreateShader(GL_VERTEX_SHADER);
    gl->glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
    gl->glCompileShader(vertexShader);

    // Check vertex shader compilation
    gl->glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        gl->glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        qCritical() << "Vertex shader compilation failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        return false;
    }

    // Create fragment shader
    GLuint fragmentShader = gl->glCreateShader(GL_FRAGMENT_SHADER);
    gl->glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
    gl->glCompileShader(fragmentShader);

    // Check fragment shader compilation
    gl->glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        gl->glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        qCritical() << "Fragment shader compilation failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);
        return false;
    }

    // Create shader program
    m_shaderProgram = gl->glCreateProgram();
    gl->glAttachShader(m_shaderProgram, vertexShader);
    gl->glAttachShader(m_shaderProgram, fragmentShader);
    gl->glLinkProgram(m_shaderProgram);

    // Check program linking
    gl->glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        gl->glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        qCritical() << "Shader program linking failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);
        gl->glDeleteProgram(m_shaderProgram);
        return false;
    }

    // Clean up shaders (no longer needed after linking)
    gl->glDeleteShader(vertexShader);
    gl->glDeleteShader(fragmentShader);

    // Get uniform locations
    m_mvpLocation = gl->glGetUniformLocation(m_shaderProgram, "mvp");

    return true;
}

#ifdef Q_OS_ANDROID
bool PointCloudRenderer::createBuffers(QOpenGLExtraFunctions *gl)
#else
bool PointCloudRenderer::createBuffers(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    // Create VAO
    gl->glGenVertexArrays(1, &m_vao);
    gl->glBindVertexArray(m_vao);

    // Create VBO
    gl->glGenBuffers(1, &m_vbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // Position attribute (location 0)
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // Intensity attribute (location 1)
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));

    gl->glBindVertexArray(0);

    return true;
}

#ifdef Q_OS_ANDROID
bool PointCloudRenderer::createAxisShaders(QOpenGLExtraFunctions *gl)
#else
bool PointCloudRenderer::createAxisShaders(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    GLint success;
    GLchar infoLog[512];

    // Create vertex shader
    GLuint vertexShader = gl->glCreateShader(GL_VERTEX_SHADER);
    gl->glShaderSource(vertexShader, 1, &axisVertexShaderSource, nullptr);
    gl->glCompileShader(vertexShader);

    gl->glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        gl->glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        qCritical() << "Axis vertex shader compilation failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        return false;
    }

    // Create fragment shader
    GLuint fragmentShader = gl->glCreateShader(GL_FRAGMENT_SHADER);
    gl->glShaderSource(fragmentShader, 1, &axisFragmentShaderSource, nullptr);
    gl->glCompileShader(fragmentShader);

    gl->glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        gl->glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        qCritical() << "Axis fragment shader compilation failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);
        return false;
    }

    // Create shader program
    m_axisShaderProgram = gl->glCreateProgram();
    gl->glAttachShader(m_axisShaderProgram, vertexShader);
    gl->glAttachShader(m_axisShaderProgram, fragmentShader);
    gl->glLinkProgram(m_axisShaderProgram);

    gl->glGetProgramiv(m_axisShaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        gl->glGetProgramInfoLog(m_axisShaderProgram, 512, nullptr, infoLog);
        qCritical() << "Axis shader program linking failed:" << infoLog;
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);
        gl->glDeleteProgram(m_axisShaderProgram);
        return false;
    }

    gl->glDeleteShader(vertexShader);
    gl->glDeleteShader(fragmentShader);

    // Get uniform locations
    m_axisMvpLocation = gl->glGetUniformLocation(m_axisShaderProgram, "mvp");
    m_axisColorLocation = gl->glGetUniformLocation(m_axisShaderProgram, "color");

    return true;
}

#ifdef Q_OS_ANDROID
bool PointCloudRenderer::createAxisBuffers(QOpenGLExtraFunctions *gl)
#else
bool PointCloudRenderer::createAxisBuffers(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    // Create axis lines: X (red), Y (green), Z (blue)
    // Each axis is a line from origin to positive direction
    float axisVertices[] = {
        // X-axis (red) - origin to +X
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        // Y-axis (green) - origin to +Y
        0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        // Z-axis (blue) - origin to +Z
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    gl->glGenVertexArrays(1, &m_axisVao);
    gl->glBindVertexArray(m_axisVao);

    gl->glGenBuffers(1, &m_axisVbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_axisVbo);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(axisVertices), axisVertices, GL_STATIC_DRAW);

    // Position attribute
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    gl->glBindVertexArray(0);

    return true;
}

void PointCloudRenderer::generateDummyPointCloud(size_t numPoints)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-5.0f, 5.0f);

    std::vector<float> points;
    points.reserve(numPoints * 3);

    for (size_t i = 0; i < numPoints; ++i) {
        points.push_back(dist(gen)); // x
        points.push_back(dist(gen)); // y
        points.push_back(dist(gen)); // z
    }

    loadStaticPointCloud(points);
}

void PointCloudRenderer::updatePointCloud(const std::vector<float> &points)
{
    if (points.empty() || points.size() % 3 != 0) {
        qWarning() << "Invalid point cloud data: empty or not multiple of 3. Size:" << points.size();
        return;
    }

    size_t newPoints = points.size() / 3;
    qDebug() << "Received new point cloud frame with" << newPoints << "points";

    // Add new frame to the deque
    m_pointFrames.emplace_back(points);

    // Mark that buffer needs update
    m_bufferNeedsUpdate = true;

    // Apply point limit - remove oldest frames if exceeded
    size_t currentPoints = 0;
    for (const auto& frame : m_pointFrames) {
        currentPoints += frame.positions.size() / 3;
    }

    while (currentPoints > m_maxPoints && !m_pointFrames.empty()) {
        size_t oldestFramePoints = m_pointFrames.front().positions.size() / 3;
        m_pointFrames.pop_front();
        currentPoints -= oldestFramePoints;
    }

    // Update total points count immediately
    m_totalPoints = currentPoints;

    qDebug() << "Total accumulated points:" << m_totalPoints << "in" << m_pointFrames.size() << "frames";
}

void PointCloudRenderer::loadStaticPointCloud(const std::vector<float> &points)
{
    // Clear accumulated data
    m_pointFrames.clear();

    if (!points.empty() && points.size() % 3 == 0) {
        // Add as single frame
        m_pointFrames.emplace_back(points);
        m_totalPoints = points.size() / 3;
    } else {
        m_totalPoints = 0;
    }

    m_bufferNeedsUpdate = true;
}

void PointCloudRenderer::clearAccumulatedPoints()
{
    m_pointFrames.clear();
    m_bufferData.clear();
    m_totalPoints = 0;
    m_bufferNeedsUpdate = true;
}

void PointCloudRenderer::applyDecay()
{
    if (!m_decayEnabled || m_pointFrames.empty()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Remove expired frames from the front
    while (!m_pointFrames.empty()) {
        const auto& oldestFrame = m_pointFrames.front();
        auto age = std::chrono::duration<float>(now - oldestFrame.timestamp).count();

        if (age > m_decayTime) {
            m_pointFrames.pop_front();
            m_bufferNeedsUpdate = true;
        } else {
            break;  // Frames are ordered by time, so we can stop
        }
    }
}

void PointCloudRenderer::rebuildBufferData()
{
    if (m_pointFrames.empty()) {
        m_bufferData.clear();
        m_totalPoints = 0;
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Pre-calculate total size
    size_t totalPoints = 0;
    for (const auto& frame : m_pointFrames) {
        totalPoints += frame.positions.size() / 3;
    }

    m_bufferData.clear();
    m_bufferData.reserve(totalPoints * 4); // x, y, z, intensity

    // Build buffer with intensity based on age
    for (const auto& frame : m_pointFrames) {
        float intensity = 1.0f;

        if (m_decayEnabled) {
            auto age = std::chrono::duration<float>(now - frame.timestamp).count();
            intensity = 1.0f - (age / m_decayTime);
            intensity = qBound(0.0f, intensity, 1.0f);
        }

        // Add all points from this frame with calculated intensity
        for (size_t i = 0; i < frame.positions.size(); i += 3) {
            m_bufferData.push_back(frame.positions[i]);     // x
            m_bufferData.push_back(frame.positions[i + 1]); // y
            m_bufferData.push_back(frame.positions[i + 2]); // z
            m_bufferData.push_back(intensity);              // intensity
        }
    }

    m_totalPoints = totalPoints;
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::updateVBO(QOpenGLExtraFunctions *gl)
#else
void PointCloudRenderer::updateVBO(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    if (m_bufferData.empty()) {
        return;
    }

    gl->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    gl->glBufferData(GL_ARRAY_BUFFER,
                     m_bufferData.size() * sizeof(float),
                     m_bufferData.data(),
                     GL_DYNAMIC_DRAW); // Use DYNAMIC_DRAW for frequently updated data
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::render(QOpenGLExtraFunctions *gl,
                                const QMatrix4x4 &viewMatrix,
                                const QMatrix4x4 &projectionMatrix)
#else
void PointCloudRenderer::render(QOpenGLFunctions_3_3_Core *gl,
                                const QMatrix4x4 &viewMatrix,
                                const QMatrix4x4 &projectionMatrix)
#endif
{
    if (!m_initialized) {
        return;
    }

    // Check if we have any data to render
    if (m_pointFrames.empty()) {
        return;
    }

    // Only apply decay every N frames to reduce CPU overhead
    m_framesSinceLastDecay++;
    if (m_framesSinceLastDecay >= 10) {  // Apply decay every 10 frames (~6 times per second at 60fps)
        applyDecay();
        m_framesSinceLastDecay = 0;

        // If decay removed frames, we need to rebuild
        if (m_decayEnabled) {
            m_bufferNeedsUpdate = true;
        }
    }

    // Only rebuild and upload buffer if needed
    if (m_bufferNeedsUpdate) {
        qDebug() << "Rebuilding buffer data...";
        rebuildBufferData();
        updateVBO(gl);
        m_bufferNeedsUpdate = false;
        qDebug() << "Buffer updated with" << m_totalPoints << "points";
    }

    // Safety check after rebuild
    if (m_totalPoints == 0) {
        return;
    }

    // Use shader program
    gl->glUseProgram(m_shaderProgram);

    // Set MVP matrix
    QMatrix4x4 mvp = projectionMatrix * viewMatrix;
    gl->glUniformMatrix4fv(m_mvpLocation, 1, GL_FALSE, mvp.constData());

    // Enable blending for transparency
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw points
    gl->glBindVertexArray(m_vao);
    gl->glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_totalPoints));
    gl->glBindVertexArray(0);

    gl->glDisable(GL_BLEND);
    gl->glUseProgram(0);
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::renderAxes(QOpenGLExtraFunctions *gl,
                                    const QMatrix4x4 &viewMatrix,
                                    const QMatrix4x4 &projectionMatrix,
                                    float axisLength)
#else
void PointCloudRenderer::renderAxes(QOpenGLFunctions_3_3_Core *gl,
                                    const QMatrix4x4 &viewMatrix,
                                    const QMatrix4x4 &projectionMatrix,
                                    float axisLength)
#endif
{
    if (!m_initialized) {
        return;
    }

    gl->glUseProgram(m_axisShaderProgram);

    // Scale matrix for axis length
    QMatrix4x4 scaleMatrix;
    scaleMatrix.scale(axisLength);
    QMatrix4x4 mvp = projectionMatrix * viewMatrix * scaleMatrix;
    gl->glUniformMatrix4fv(m_axisMvpLocation, 1, GL_FALSE, mvp.constData());

    gl->glBindVertexArray(m_axisVao);

    // Increase line width for visibility
    gl->glLineWidth(3.0f);

    // Draw X-axis (red)
    gl->glUniform3f(m_axisColorLocation, 1.0f, 0.0f, 0.0f);
    gl->glDrawArrays(GL_LINES, 0, 2);

    // Draw Y-axis (green)
    gl->glUniform3f(m_axisColorLocation, 0.0f, 1.0f, 0.0f);
    gl->glDrawArrays(GL_LINES, 2, 2);

    // Draw Z-axis (blue)
    gl->glUniform3f(m_axisColorLocation, 0.0f, 0.0f, 1.0f);
    gl->glDrawArrays(GL_LINES, 4, 2);

    gl->glLineWidth(1.0f);
    gl->glBindVertexArray(0);
    gl->glUseProgram(0);
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::renderPath(QOpenGLExtraFunctions *gl,
                                    const QMatrix4x4 &viewMatrix,
                                    const QMatrix4x4 &projectionMatrix,
                                    const std::vector<QVector3D> &path)
#else
void PointCloudRenderer::renderPath(QOpenGLFunctions_3_3_Core *gl,
                                    const QMatrix4x4 &viewMatrix,
                                    const QMatrix4x4 &projectionMatrix,
                                    const std::vector<QVector3D> &path)
#endif
{
    if (!m_initialized || path.empty()) {
        return;
    }

    // Always refresh path buffer because coordinates change even when path
    // length stays constant (sliding history / tail behavior).
    std::vector<float> pathData;
    pathData.reserve(path.size() * 3);

    for (const auto& p : path) {
        pathData.push_back(p.x());
        pathData.push_back(p.y());
        pathData.push_back(p.z());
    }

    gl->glBindVertexArray(m_pathVao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_pathVbo);
    gl->glBufferData(GL_ARRAY_BUFFER, pathData.size() * sizeof(float),
                    pathData.data(), GL_DYNAMIC_DRAW);

    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    gl->glBindVertexArray(0);
    m_pathPointCount = path.size();

    // Render path as line strip (yellow/orange color)
    gl->glUseProgram(m_axisShaderProgram);

    QMatrix4x4 mvp = projectionMatrix * viewMatrix;
    gl->glUniformMatrix4fv(m_axisMvpLocation, 1, GL_FALSE, mvp.constData());
    gl->glUniform3f(m_axisColorLocation, 1.0f, 0.8f, 0.0f); // Yellow/orange

    gl->glLineWidth(2.0f);
    gl->glBindVertexArray(m_pathVao);
    gl->glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_pathPointCount));
    gl->glBindVertexArray(0);
    gl->glLineWidth(1.0f);

    gl->glUseProgram(0);
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::renderRobotFrame(QOpenGLExtraFunctions *gl,
                                          const QMatrix4x4 &viewMatrix,
                                          const QMatrix4x4 &projectionMatrix,
                                          const QVector3D &position,
                                          const QQuaternion &orientation,
                                          float axisLength)
#else
void PointCloudRenderer::renderRobotFrame(QOpenGLFunctions_3_3_Core *gl,
                                          const QMatrix4x4 &viewMatrix,
                                          const QMatrix4x4 &projectionMatrix,
                                          const QVector3D &position,
                                          const QQuaternion &orientation,
                                          float axisLength)
#endif
{
    if (!m_initialized) {
        return;
    }

    // Create transform matrix: translate to position, rotate by orientation, scale by axis length
    QMatrix4x4 transform;
    transform.translate(position);
    transform.rotate(orientation);
    transform.scale(axisLength);

    QMatrix4x4 mvp = projectionMatrix * viewMatrix * transform;

    gl->glUseProgram(m_axisShaderProgram);
    gl->glUniformMatrix4fv(m_axisMvpLocation, 1, GL_FALSE, mvp.constData());

    gl->glBindVertexArray(m_axisVao);
    gl->glLineWidth(4.0f); // Thicker for robot frame

    // Draw robot's local X-axis (red) - forward
    gl->glUniform3f(m_axisColorLocation, 1.0f, 0.0f, 0.0f);
    gl->glDrawArrays(GL_LINES, 0, 2);

    // Draw robot's local Y-axis (green) - left
    gl->glUniform3f(m_axisColorLocation, 0.0f, 1.0f, 0.0f);
    gl->glDrawArrays(GL_LINES, 2, 2);

    // Draw robot's local Z-axis (blue) - up
    gl->glUniform3f(m_axisColorLocation, 0.0f, 0.0f, 1.0f);
    gl->glDrawArrays(GL_LINES, 4, 2);

    gl->glLineWidth(1.0f);
    gl->glBindVertexArray(0);
    gl->glUseProgram(0);
}

#ifdef Q_OS_ANDROID
void PointCloudRenderer::cleanup(QOpenGLExtraFunctions *gl)
#else
void PointCloudRenderer::cleanup(QOpenGLFunctions_3_3_Core *gl)
#endif
{
    if (!m_initialized) {
        return;
    }

    if (gl) {
        if (m_vao != 0) {
            gl->glDeleteVertexArrays(1, &m_vao);
            m_vao = 0;
        }
        if (m_vbo != 0) {
            gl->glDeleteBuffers(1, &m_vbo);
            m_vbo = 0;
        }
        if (m_shaderProgram != 0) {
            gl->glDeleteProgram(m_shaderProgram);
            m_shaderProgram = 0;
        }
        if (m_axisVao != 0) {
            gl->glDeleteVertexArrays(1, &m_axisVao);
            m_axisVao = 0;
        }
        if (m_axisVbo != 0) {
            gl->glDeleteBuffers(1, &m_axisVbo);
            m_axisVbo = 0;
        }
        if (m_axisShaderProgram != 0) {
            gl->glDeleteProgram(m_axisShaderProgram);
            m_axisShaderProgram = 0;
        }
        if (m_pathVao != 0) {
            gl->glDeleteVertexArrays(1, &m_pathVao);
            m_pathVao = 0;
        }
        if (m_pathVbo != 0) {
            gl->glDeleteBuffers(1, &m_pathVbo);
            m_pathVbo = 0;
        }
    }

    m_initialized = false;
}

// ============================================================================
// PointCloudWidget Implementation
// ============================================================================

PointCloudWidget::PointCloudWidget(QWidget *parent)
    : QOpenGLWidget(parent)
    , m_robotPosition(0, 0, 0)
    , m_robotOrientation(1, 0, 0, 0)
    , m_hasRobotPose(false)
    , m_leftButtonPressed(false)
    , m_rightButtonPressed(false)
{
#ifdef Q_OS_ANDROID
    // FIX: initialize all new touch state members
    m_lastTouchPos      = QPointF();
    m_lastPinchDistance = 0.0;
    m_lastPinchMidpoint = QPointF();
    m_lastPinchAngle    = 0.0f;
    m_isPinching             = false;
    m_lastActiveFingerCount  = 0;
    setAttribute(Qt::WA_AcceptTouchEvents);
#endif
}

PointCloudWidget::~PointCloudWidget()
{
    // CRITICAL: Make sure OpenGL context is current before cleanup
    makeCurrent();

    // Now it's safe to cleanup OpenGL resources
    m_renderer.cleanup(this);

    // Release the context
    doneCurrent();
}

bool PointCloudWidget::loadPointCloud(const QString &filename)
{
    QString actualFilename = filename;
    QString tempFilePath;

#ifdef Q_OS_ANDROID
    // On Android, if it's a content URI, copy to temp file
    if (filename.startsWith("content://")) {
        QFileInfo fileInfo(filename);
        QString extension = fileInfo.suffix().toLower();

        // Create temp file
        tempFilePath = QDir::temp().filePath("temp_pointcloud." + extension);

        // Copy content URI to temp file
        QFile sourceFile(filename);
        if (!sourceFile.open(QIODevice::ReadOnly)) {
            qWarning() << "Failed to open content URI:" << sourceFile.errorString();
            return false;
        }

        QFile destFile(tempFilePath);
        if (!destFile.open(QIODevice::WriteOnly)) {
            qWarning() << "Failed to create temp file:" << destFile.errorString();
            sourceFile.close();
            return false;
        }

        // Copy data
        destFile.write(sourceFile.readAll());
        sourceFile.close();
        destFile.close();

        actualFilename = tempFilePath;
        qInfo() << "Copied content URI to temp file:" << tempFilePath;
    }
#endif

    QFileInfo fileInfo(actualFilename);
    if (!fileInfo.exists()) {
        qWarning() << "File does not exist:" << actualFilename;
        return false;
    }

    std::string stdFilename = actualFilename.toStdString();
    QString extension = fileInfo.suffix().toLower();
    PointCloudData data;
    bool success = false;

    qInfo() << "FILE: " << actualFilename;

    if (extension == "pcd") {
        success = PCDLoader::loadPCD(stdFilename, data);
    } else if (extension == "xyz") {
        success = PCDLoader::loadXYZ(stdFilename, data);
    } else if (extension == "txt") {
        success = PCDLoader::loadXYZRGB(stdFilename, data);
        if (!success) {
            success = PCDLoader::loadXYZ(stdFilename, data);
        }
    } else {
        qWarning() << "Unsupported file format:" << extension;
        qWarning() << "Supported formats: .pcd, .xyz, .txt";
        return false;
    }

    // Clean up temp file on Android
#ifdef Q_OS_ANDROID
    if (!tempFilePath.isEmpty()) {
        QFile::remove(tempFilePath);
    }
#endif

    if (!success) {
        qWarning() << "Failed to load point cloud:"
                   << QString::fromStdString(PCDLoader::getLastError());
        return false;
    }

    qInfo() << "Loaded" << data.pointCount << "points from" << filename;
    qInfo() << "Bounding box: min" << data.minBounds << "max" << data.maxBounds;

    m_boundingBoxMin = data.minBounds;
    m_boundingBoxMax = data.maxBounds;

    makeCurrent();
    // Load as static point cloud (replaces accumulated data)
    m_renderer.loadStaticPointCloud(data.positions);
    doneCurrent();

    QVector3D center = (data.minBounds + data.maxBounds) * 0.5f;
    QVector3D size = data.maxBounds - data.minBounds;
    float maxDim = qMax(qMax(size.x(), size.y()), size.z());

    m_camera.setTarget(center);
    m_camera.setDistance(maxDim * 2.0f);

    update();
    return true;
}

void PointCloudWidget::initializeGL()
{
    // Initialize OpenGL functions
    initializeOpenGLFunctions();

    qInfo() << "OpenGL Version:" << (const char*)glGetString(GL_VERSION);
    qInfo() << "GLSL Version:" << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);

    // Set clear color (dark background)
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    // Enable depth testing
    glEnable(GL_DEPTH_TEST);

    // Enable point size control from vertex shader
#ifndef Q_OS_ANDROID
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif

    // Initialize renderer
    if (!m_renderer.initialize(this)) {
        qCritical() << "Failed to initialize renderer";
        return;
    }

    qInfo() << "OpenGL initialized successfully";
    qInfo() << "Point count:" << m_renderer.getPointCount();
}

void PointCloudWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void PointCloudWidget::paintGL()
{
    // Clear buffers
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Get camera matrices
    float aspectRatio = static_cast<float>(width()) / static_cast<float>(height());
    QMatrix4x4 viewMatrix = m_camera.getViewMatrix();
    QMatrix4x4 projectionMatrix = m_camera.getProjectionMatrix(aspectRatio);

    // Calculate axis length based on scene bounds
    QVector3D size = m_boundingBoxMax - m_boundingBoxMin;
    float sceneSize = qMax(qMax(size.x(), size.y()), size.z());
    float axisLength = qMax(1.0f, sceneSize * 0.3f);  // 30% of scene size, minimum 1.0

    // Render coordinate axes first (so they appear behind points)
    m_renderer.renderAxes(this, viewMatrix, projectionMatrix, axisLength);

    // Render path if available
    if (!m_path.empty()) {
        m_renderer.renderPath(this, viewMatrix, projectionMatrix, m_path);
    }

    // Render robot frame if available
    if (m_hasRobotPose) {
        float robotAxisLength = qMax(0.5f, sceneSize * 0.15f); // Smaller than origin axes
        m_renderer.renderRobotFrame(this, viewMatrix, projectionMatrix,
                                    m_robotPosition, m_robotOrientation, robotAxisLength);
    }

    // Render point cloud
    m_renderer.render(this, viewMatrix, projectionMatrix);
}

void PointCloudWidget::updatePath(const std::vector<QVector3D> &path)
{
    m_path = path;
    update(); // Trigger repaint
}

void PointCloudWidget::updateRobotPose(const QVector3D &position, const QQuaternion &orientation)
{
    m_robotPosition = position;
    m_robotOrientation = orientation;
    m_hasRobotPose = true;
    update(); // Trigger repaint
}

// ============================================================================
// Desktop mouse events (unchanged — work correctly on Windows)
// ============================================================================

void PointCloudWidget::mousePressEvent(QMouseEvent *event)
{
    m_lastMousePos = event->pos();

    if (event->button() == Qt::LeftButton) {
        m_leftButtonPressed = true;
    }
    else if (event->button() == Qt::RightButton) {
        m_rightButtonPressed = true;
    }
}

void PointCloudWidget::mouseMoveEvent(QMouseEvent *event)
{
    QPoint currentPos = event->pos();
    QPoint delta = currentPos - m_lastMousePos;

    if (m_leftButtonPressed) {
        // Rotate camera
        float rotationSpeed = 0.5f;
        m_camera.rotate(-delta.x() * rotationSpeed, delta.y() * rotationSpeed);
        update();
    }
    else if (m_rightButtonPressed) {
        // FIX: scale pan speed by camera distance so it feels consistent at all zoom levels
        float panSpeed = m_camera.distance() * 0.001f;
        m_camera.pan(-delta.x() * panSpeed, delta.y() * panSpeed);
        update();
    }

    m_lastMousePos = currentPos;
}

void PointCloudWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_leftButtonPressed = false;
    }
    else if (event->button() == Qt::RightButton) {
        m_rightButtonPressed = false;
    }
}

void PointCloudWidget::wheelEvent(QWheelEvent *event)
{
    // FIX: multiplicative zoom — each scroll notch scales distance by 10%.
    // This feels natural at any zoom level (near or far).
    // Old code used additive delta which was tuned for mouse and broke on touch.
    float steps = event->angleDelta().y() / 120.0f; // one standard notch = 120 units
    float factor = qPow(0.9f, steps);               // 0.9^steps per notch
    m_camera.setDistance(m_camera.distance() * factor);
    update();
}

// ============================================================================
// Android touch event handler — completely rewritten
// ============================================================================

#ifdef Q_OS_ANDROID
bool PointCloudWidget::event(QEvent *event)
{
    switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        {
            QTouchEvent *touchEvent = static_cast<QTouchEvent *>(event);
            const QList<QEventPoint> &points = touchEvent->points();

            // Count only fingers that are still actively pressing
            // (not already released in this same event batch)
            int activeCount = 0;
            for (const QEventPoint &pt : points) {
                if (pt.state() != QEventPoint::Released) {
                    ++activeCount;
                }
            }

            const float dpr = static_cast<float>(devicePixelRatio());

            // -----------------------------------------------------------------
            // ROOT CAUSE FIX: "jump on finger lift"
            //
            // When lifting one finger after a 2-finger gesture, Qt fires a
            // TouchUpdate with points.count()==1 BUT the remaining finger's
            // position() is unrelated to where the previous 1-finger pan
            // baseline (m_lastTouchPos) was. Computing a delta against the
            // stale baseline produces a huge phantom movement.
            //
            // Solution: whenever the active finger count CHANGES, we treat
            // it as a new gesture start — reset all baselines and skip
            // computing any delta for that transition frame.
            // -----------------------------------------------------------------
            bool fingerCountChanged = (activeCount != m_lastActiveFingerCount);
            m_lastActiveFingerCount = activeCount;

            // On TouchEnd (all fingers lifted) just reset state and exit
            if (event->type() == QEvent::TouchEnd || activeCount == 0) {
                m_isPinching = false;
                m_lastActiveFingerCount = 0;
                return true;
            }

            if (activeCount == 1) {
                // Find the one active (non-released) finger
                const QEventPoint *activePt = nullptr;
                for (const QEventPoint &pt : points) {
                    if (pt.state() != QEventPoint::Released) {
                        activePt = &pt;
                        break;
                    }
                }
                if (!activePt) return true;

                if (event->type() == QEvent::TouchBegin || fingerCountChanged) {
                    // New gesture or transition from 2-finger → 1-finger:
                    // record current position as baseline, DO NOT rotate this frame.
                    m_lastTouchPos = activePt->position();
                    m_isPinching = false;
                } else {
                    QPointF rawDelta = activePt->position() - m_lastTouchPos;
                    QPointF delta = rawDelta / dpr;

                    // Standard mobile 3D behavior: one finger rotates (orbits)
                    float rotationSpeed = 0.45f;
                    m_camera.rotate(-static_cast<float>(delta.x()) * rotationSpeed,
                                     static_cast<float>(delta.y()) * rotationSpeed);
                    update();
                    m_lastTouchPos = activePt->position();
                }
            }
            else if (activeCount == 2) {
                // ---------------------------------------------------------
                // 2 fingers: simultaneous pinch-zoom + pan (standard mobile)
                // ---------------------------------------------------------
                // Collect only active (not yet released) fingers
                QVector<QPointF> activePts2;
                for (const QEventPoint &pt : points) {
                    if (pt.state() != QEventPoint::Released)
                        activePts2.append(pt.position());
                    if (activePts2.size() == 2) break;
                }
                if (activePts2.size() < 2) return true;

                QPointF p1 = activePts2[0];
                QPointF p2 = activePts2[1];

                // Distance in logical pixels
                qreal currentDist  = QLineF(p1, p2).length() / dpr;
                QPointF currentMidpoint = (p1 + p2) * 0.5;

                if (event->type() == QEvent::TouchBegin || !m_isPinching || fingerCountChanged) {
                    // New 2-finger gesture or transition from 1-finger: record baseline only
                    m_lastPinchDistance = currentDist;
                    m_lastPinchMidpoint = currentMidpoint;
                    m_isPinching        = true;
                } else {
                    // ---------------------------------------------------------
                    // FIX 4: multiplicative (ratio) zoom.
                    // Old code: zoom(-delta * 0.01f) — additive, mouse-tuned.
                    // A full pinch on phone = ~300 logical px → 3 units added,
                    // which was jarring. Ratio zoom: currentDist/lastDist
                    // gives a smooth proportional scale regardless of screen size.
                    // ---------------------------------------------------------
                    if (m_lastPinchDistance > 1.0) {
                        float zoomFactor = static_cast<float>(
                            currentDist / m_lastPinchDistance);
                        // zoomFactor > 1 means fingers spread (zoom in = decrease distance)
                        m_camera.setDistance(m_camera.distance() / zoomFactor);
                    }

                    // Midpoint motion pans the scene while maintaining zoom.
                    QPointF midDelta = (currentMidpoint - m_lastPinchMidpoint) / dpr;
                    float panSpeed = m_camera.distance() * 0.002f;
                    m_camera.pan(-static_cast<float>(midDelta.x()) * panSpeed,
                                  static_cast<float>(midDelta.y()) * panSpeed);

                    m_lastPinchDistance = currentDist;
                    m_lastPinchMidpoint = currentMidpoint;
                    update();
                }
            }
            else {
                // 3+ fingers: reset touch state to avoid stale baselines
                m_isPinching = false;
            }

            return true;
        }
        default:
            break;
    }

    return QOpenGLWidget::event(event);
}
#endif // Q_OS_ANDROID
