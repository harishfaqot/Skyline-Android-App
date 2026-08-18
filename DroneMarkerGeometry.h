#ifndef DRONEMARKERGEOMETRY_H
#define DRONEMARKERGEOMETRY_H

#include <vector>

// Procedural geometry for the small drone marker drawn at the drone's live
// pose in the point cloud (see PointCloudRenderer::renderRobotFrame in
// PointCloudWidget.cpp). Pulled into its own file, separate from the point
// cloud rendering/streaming code, so the look of the marker can be
// redesigned later (simplified, restyled, swapped out entirely) without
// touching anything else.
//
// This is a plain GL_LINES wireframe — four arms in an X-configuration
// crossing at the center (the "body" is just the X itself, no separate
// hull), each ending in a rotor guard ring with a propeller blade cross
// inside it, all enclosed by an outer protective cage (a geodesic strut
// frame, styled after collision-tolerant mapping drones like the
// Flyability Elios 3) — NOT a URDF model. URDF describes a robot's
// kinematic tree (links/joints, meshes) for full robot visualization
// (RViz, Gazebo, robot_state_publisher) and needs an XML parser, mesh
// loading and a kinematic solver; this marker is a single rigid,
// non-articulated shape rendered as a handful of line segments with the
// app's existing solid-color shader, so pulling in URDF would add a lot of
// machinery this app has no other use for. If the drone ever needs a
// proper articulated/detailed model, that's the point to reach for
// something like that — this file is intentionally just plain geometry.
namespace DroneMarkerGeometry {

// Vertex layout: offsets/counts below describe where each part lives in
// the buffer returned by buildVertices(), in GL_LINES pairs. Every vertex
// is 3 floats (x, y, z), in local unit space — the caller (renderRobotFrame)
// is responsible for translating to the drone's position, rotating by its
// orientation, and scaling to the desired on-screen size.

// Arms: one line per arm, from the center out to the rotor tip. Four arms
// in an X-configuration crossing at the origin form the whole "body" — no
// separate hull shape.
constexpr int kArmCount = 4;
constexpr int kArmVertexOffset = 0;
constexpr int kArmVertexCount = kArmCount * 2; // 8

// Rotor guard: one circle per arm tip.
constexpr int kRotorSegments = 10;
constexpr int kRotorVertexOffset = kArmVertexOffset + kArmVertexCount;
constexpr int kRotorVertexCount = kArmCount * kRotorSegments * 2; // 80

// Propeller blades: a small cross (two short lines) inside each rotor
// guard circle, so the guard ring reads as sitting *around* a propeller
// rather than being the propeller itself.
constexpr int kBladeVertexOffset = kRotorVertexOffset + kRotorVertexCount;
constexpr int kBladeVertexCount = kArmCount * 4; // 2 lines x 2 verts, per tip

// Outer protective cage: a full enclosing sphere (top pole, bottom pole,
// and horizontal ring "latitudes" in between) rather than a coarse
// polygon — smooth circles for the latitude rings, plus meridian struts
// connecting pole -> ring -> ring -> ring -> pole, so it reads as round
// rather than faceted, while still keeping the strut/cage look of the
// real drone's protective frame.
constexpr int kDomeRingSegments = 24;             // smoothness of each latitude circle
constexpr int kDomeRingVertexCount = kDomeRingSegments * 2;
constexpr int kDomeRingCount = 5;                 // latitude bands between the two poles
constexpr int kDomeMeridianCount = 12;            // longitude struts, evenly spaced
constexpr int kDomeMeridianSegments = 6;          // pole -> ring -> ring -> ring -> ring -> ring -> pole

constexpr int kCageVertexOffset = kBladeVertexOffset + kBladeVertexCount;
constexpr int kCageRingVertexCount = kDomeRingCount * kDomeRingVertexCount; // 240
constexpr int kCageMeridianVertexCount = kDomeMeridianCount * kDomeMeridianSegments * 2; // 144
constexpr int kCageVertexCount = kCageRingVertexCount + kCageMeridianVertexCount; // 384

constexpr int kTotalVertexCount = kCageVertexOffset + kCageVertexCount;

// Builds the full position-only vertex buffer (3 floats/vertex), laid out
// in the order described by the offsets/counts above.
std::vector<float> buildVertices();

} // namespace DroneMarkerGeometry

#endif // DRONEMARKERGEOMETRY_H
