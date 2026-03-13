#ifndef PCDLOADER_H
#define PCDLOADER_H

#include <vector>
#include <string>
#include <QVector3D>

struct PointCloudData {
    std::vector<float> positions;  // x, y, z interleaved
    std::vector<float> colors;     // r, g, b interleaved (optional)
    size_t pointCount;
    bool hasColor;
    
    // Bounding box
    QVector3D minBounds;
    QVector3D maxBounds;
    
    PointCloudData() : pointCount(0), hasColor(false) {}
};

class PCDLoader
{
public:
    PCDLoader();
    
    // Load PCD file (supports ASCII and binary format)
    static bool loadPCD(const std::string &filename, PointCloudData &data);
    
    // Load simple XYZ text file (one point per line: x y z)
    static bool loadXYZ(const std::string &filename, PointCloudData &data);
    
    // Load simple XYZRGB text file (one point per line: x y z r g b)
    static bool loadXYZRGB(const std::string &filename, PointCloudData &data);
    
    // Get last error message
    static std::string getLastError() { return s_lastError; }
    
private:
    static bool loadPCDAscii(std::ifstream &file, PointCloudData &data, size_t numPoints, bool hasColor);
    static bool loadPCDBinary(std::ifstream &file, PointCloudData &data, size_t numPoints, bool hasColor);
    static void computeBoundingBox(PointCloudData &data);
    
    static std::string s_lastError;
};

#endif // PCDLOADER_H
