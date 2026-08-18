#include "DroneMarkerGeometry.h"

#include <QtMath>

namespace DroneMarkerGeometry {

std::vector<float> buildVertices()
{
    std::vector<float> verts;
    verts.reserve(kTotalVertexCount * 3);

    // Arm directions, X-configuration. Pre-normalized (1/sqrt(2) per axis)
    // since all four are 45-degree diagonals.
    const float armLen = 0.65f;
    const float kDiag = 0.7071f;
    const float arms[kArmCount][2] = {
        { kDiag,  kDiag}, // front-left
        { kDiag, -kDiag}, // front-right
        {-kDiag,  kDiag}, // rear-left
        {-kDiag, -kDiag}, // rear-right
    };

    // Arms: from the center out to each rotor tip — the crossing arms
    // themselves are the whole "body", no separate hull shape.
    for (auto &dir : arms) {
        verts.insert(verts.end(), {
            0.0f, 0.0f, 0.0f,
            dir[0] * armLen, dir[1] * armLen, 0.0f,
        });
    }

    // Rotor guard circle at each arm tip.
    const float rotorRadius = 0.45f;
    for (auto &dir : arms) {
        const float tipX = dir[0] * armLen, tipY = dir[1] * armLen;
        for (int i = 0; i < kRotorSegments; ++i) {
            const float a0 = qDegreesToRadians(360.0f * i / kRotorSegments);
            const float a1 = qDegreesToRadians(360.0f * (i + 1) / kRotorSegments);
            verts.insert(verts.end(), {
                tipX + rotorRadius * qCos(a0), tipY + rotorRadius * qSin(a0), 0.0f,
                tipX + rotorRadius * qCos(a1), tipY + rotorRadius * qSin(a1), 0.0f,
            });
        }
    }

    // Propeller blade cross, sitting inside each rotor guard circle so the
    // guard clearly reads as surrounding a propeller rather than being one.
    const float bladeHalfLen = rotorRadius * 0.8f;
    for (auto &dir : arms) {
        const float tipX = dir[0] * armLen, tipY = dir[1] * armLen;
        verts.insert(verts.end(), {
            tipX - bladeHalfLen, tipY, 0.0f,
            tipX + bladeHalfLen, tipY, 0.0f,
        });
        verts.insert(verts.end(), {
            tipX, tipY - bladeHalfLen, 0.0f,
            tipX, tipY + bladeHalfLen, 0.0f,
        });
    }

    // Outer protective cage — a proper round sphere (not a coarse
    // polygon): a top pole, a bottom pole, and several horizontal latitude
    // rings drawn as smooth circles in between, all tied together by
    // meridian struts running pole -> ring -> ... -> ring -> pole. This
    // keeps the strut/cage look of the real drone's frame while actually
    // reading as round instead of faceted.
    struct Vec3 { float x, y, z; };
    const float sphereRadius = 1.1f; // sits just outside the rotor guards

    // Latitude angles, evenly spaced from the top pole (+90) to the
    // bottom pole (-90), with kDomeRingCount rings in between.
    float latDeg[kDomeRingCount + 2];
    for (int i = 0; i < kDomeRingCount + 2; ++i) {
        latDeg[i] = 90.0f - 180.0f * i / (kDomeRingCount + 1);
    }

    auto latPoint = [&](float latitudeDeg, float lonDeg) -> Vec3 {
        const float lat = qDegreesToRadians(latitudeDeg);
        const float lon = qDegreesToRadians(lonDeg);
        const float r = sphereRadius * qCos(lat);
        return {r * qCos(lon), r * qSin(lon), sphereRadius * qSin(lat)};
    };

    auto addEdge = [&](const Vec3 &a, const Vec3 &b) {
        verts.insert(verts.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
    };

    // Latitude rings (skip the poles — those are single points, not rings).
    for (int ring = 0; ring < kDomeRingCount; ++ring) {
        const float lat = latDeg[ring + 1];
        for (int i = 0; i < kDomeRingSegments; ++i) {
            const float lon0 = 360.0f * i / kDomeRingSegments;
            const float lon1 = 360.0f * (i + 1) / kDomeRingSegments;
            addEdge(latPoint(lat, lon0), latPoint(lat, lon1));
        }
    }

    // Meridian struts: pole -> upper ring -> equator -> lower ring -> pole,
    // repeated at kDomeMeridianCount evenly spaced longitudes.
    for (int m = 0; m < kDomeMeridianCount; ++m) {
        const float lon = 360.0f * m / kDomeMeridianCount;
        for (int seg = 0; seg < kDomeMeridianSegments; ++seg) {
            addEdge(latPoint(latDeg[seg], lon), latPoint(latDeg[seg + 1], lon));
        }
    }

    return verts;
}

} // namespace DroneMarkerGeometry
