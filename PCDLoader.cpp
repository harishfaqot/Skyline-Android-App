#include "PCDLoader.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <limits>

std::string PCDLoader::s_lastError;

PCDLoader::PCDLoader()
{
}

bool PCDLoader::loadPCD(const std::string &filename, PointCloudData &data)
{
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        s_lastError = "Failed to open file: " + filename;
        return false;
    }
    
    // Read header
    std::string line;
    std::string version;
    std::string fields;
    std::string dataType = "ascii";
    size_t numPoints = 0;
    bool hasColor = false;
    
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        iss >> key;
        
        if (key == "VERSION") {
            iss >> version;
        }
        else if (key == "FIELDS") {
            std::getline(iss, fields);
            // Check if has RGB/RGBA
            if (fields.find("rgb") != std::string::npos || 
                fields.find("rgba") != std::string::npos) {
                hasColor = true;
            }
        }
        else if (key == "POINTS") {
            iss >> numPoints;
        }
        else if (key == "DATA") {
            iss >> dataType;
            std::transform(dataType.begin(), dataType.end(), dataType.begin(), ::tolower);
            break;
        }
    }
    
    if (numPoints == 0) {
        s_lastError = "Invalid PCD file: no points specified";
        return false;
    }
    
    // Load data based on format
    bool success = false;
    if (dataType == "ascii") {
        success = loadPCDAscii(file, data, numPoints, hasColor);
    } else if (dataType == "binary") {
        success = loadPCDBinary(file, data, numPoints, hasColor);
    } else {
        s_lastError = "Unsupported data type: " + dataType;
        return false;
    }
    
    if (success) {
        computeBoundingBox(data);
    }
    
    return success;
}

bool PCDLoader::loadPCDAscii(std::ifstream &file, PointCloudData &data, size_t numPoints, bool hasColor)
{
    data.positions.reserve(numPoints * 3);
    if (hasColor) {
        data.colors.reserve(numPoints * 3);
    }
    
    std::string line;
    size_t pointsRead = 0;
    
    while (std::getline(file, line) && pointsRead < numPoints) {
        std::istringstream iss(line);
        float x, y, z;
        
        if (!(iss >> x >> y >> z)) {
            continue; // Skip invalid lines
        }
        
        data.positions.push_back(x);
        data.positions.push_back(y);
        data.positions.push_back(z);
        
        if (hasColor) {
            // Try to read RGB (can be float or uint32_t packed)
            float r, g, b;
            if (iss >> r >> g >> b) {
                // Normalize if values are 0-255
                if (r > 1.0f || g > 1.0f || b > 1.0f) {
                    r /= 255.0f;
                    g /= 255.0f;
                    b /= 255.0f;
                }
                data.colors.push_back(r);
                data.colors.push_back(g);
                data.colors.push_back(b);
            } else {
                // Default color if not available
                data.colors.push_back(0.7f);
                data.colors.push_back(0.7f);
                data.colors.push_back(0.7f);
            }
        }
        
        pointsRead++;
    }
    
    data.pointCount = pointsRead;
    data.hasColor = hasColor;
    
    return pointsRead > 0;
}

bool PCDLoader::loadPCDBinary(std::ifstream &file, PointCloudData &data, size_t numPoints, bool hasColor)
{
    // Simple binary format: assume float x, y, z (and optionally rgb as uint32)
    data.positions.reserve(numPoints * 3);
    if (hasColor) {
        data.colors.reserve(numPoints * 3);
    }
    
    size_t pointSize = hasColor ? 16 : 12; // 3 floats or 3 floats + 1 uint32
    
    for (size_t i = 0; i < numPoints; ++i) {
        float xyz[3];
        file.read(reinterpret_cast<char*>(xyz), 3 * sizeof(float));
        
        if (!file) {
            break;
        }
        
        data.positions.push_back(xyz[0]);
        data.positions.push_back(xyz[1]);
        data.positions.push_back(xyz[2]);
        
        if (hasColor) {
            uint32_t rgb;
            file.read(reinterpret_cast<char*>(&rgb), sizeof(uint32_t));
            
            // Extract RGB from packed uint32 (assuming BGR order)
            float r = ((rgb >> 16) & 0xFF) / 255.0f;
            float g = ((rgb >> 8) & 0xFF) / 255.0f;
            float b = (rgb & 0xFF) / 255.0f;
            
            data.colors.push_back(r);
            data.colors.push_back(g);
            data.colors.push_back(b);
        }
    }
    
    data.pointCount = data.positions.size() / 3;
    data.hasColor = hasColor;
    
    return data.pointCount > 0;
}

bool PCDLoader::loadXYZ(const std::string &filename, PointCloudData &data)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        s_lastError = "Failed to open file: " + filename;
        return false;
    }
    
    data.positions.clear();
    data.hasColor = false;
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        std::istringstream iss(line);
        float x, y, z;
        
        if (iss >> x >> y >> z) {
            data.positions.push_back(x);
            data.positions.push_back(y);
            data.positions.push_back(z);
        }
    }
    
    data.pointCount = data.positions.size() / 3;
    
    if (data.pointCount == 0) {
        s_lastError = "No valid points found in file";
        return false;
    }
    
    computeBoundingBox(data);
    return true;
}

bool PCDLoader::loadXYZRGB(const std::string &filename, PointCloudData &data)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        s_lastError = "Failed to open file: " + filename;
        return false;
    }
    
    data.positions.clear();
    data.colors.clear();
    data.hasColor = true;
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        std::istringstream iss(line);
        float x, y, z, r, g, b;
        
        if (iss >> x >> y >> z >> r >> g >> b) {
            data.positions.push_back(x);
            data.positions.push_back(y);
            data.positions.push_back(z);
            
            // Normalize colors if they're in 0-255 range
            if (r > 1.0f || g > 1.0f || b > 1.0f) {
                r /= 255.0f;
                g /= 255.0f;
                b /= 255.0f;
            }
            
            data.colors.push_back(r);
            data.colors.push_back(g);
            data.colors.push_back(b);
        }
    }
    
    data.pointCount = data.positions.size() / 3;
    
    if (data.pointCount == 0) {
        s_lastError = "No valid points found in file";
        return false;
    }
    
    computeBoundingBox(data);
    return true;
}

void PCDLoader::computeBoundingBox(PointCloudData &data)
{
    if (data.positions.empty()) {
        return;
    }
    
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    float maxZ = std::numeric_limits<float>::lowest();
    
    for (size_t i = 0; i < data.positions.size(); i += 3) {
        float x = data.positions[i];
        float y = data.positions[i + 1];
        float z = data.positions[i + 2];
        
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        minZ = std::min(minZ, z);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        maxZ = std::max(maxZ, z);
    }
    
    data.minBounds = QVector3D(minX, minY, minZ);
    data.maxBounds = QVector3D(maxX, maxY, maxZ);
}
