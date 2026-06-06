/*
 * Math helpers for PBRT scene builder.
 */
#pragma once

#include <pbrtio/PbrtCompat.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

namespace pbrtio::pbrt {

constexpr float kPi = 3.14159265358979323846f;

inline float radians(float degrees) {
    return degrees * kPi / 180.f;
}

inline float4x4 matrixFromRotation(float angleRad, float3 axis) {
    return glm::rotate(float4x4(1.f), angleRad, normalize(axis));
}

inline float4x4 matrixFromTranslation(float3 t) {
    return glm::translate(float4x4(1.f), t);
}

inline float4x4 matrixFromScaling(float3 s) {
    return glm::scale(float4x4(1.f), s);
}

inline float4x4 matrixFromLookAt(float3 eye, float3 at, float3 up) {
    float3 zAxis = normalize(at - eye);
    float3 normUp = normalize(up);
    if (std::abs(dot(zAxis, normUp)) > 0.999f) {
        normUp = float3(normUp.z, normUp.x, normUp.y);
    }
    const float3 xAxis = normalize(cross(zAxis, normUp));
    const float3 yAxis = cross(xAxis, zAxis);
    return float4x4(
            float4(xAxis, 0.f),
            float4(yAxis, 0.f),
            float4(-zAxis, 0.f),
            float4(eye, 1.f));
}

inline float4x4 float4x4FromPbrtRowMajor(const Float* tr) {
    return float4x4(tr[0], tr[1], tr[2], tr[3], tr[4], tr[5], tr[6], tr[7], tr[8], tr[9], tr[10], tr[11],
            tr[12], tr[13], tr[14], tr[15]);
}

inline float4x4 pbrtRowMajorTransform(const Float* tr) {
    // The 16-scalar constructor fills columns. Passing PBRT's row-vector
    // matrix directly therefore produces the column-vector equivalent.
    return float4x4FromPbrtRowMajor(tr);
}

inline std::string to_string(const float4x4& m) {
    (void)m;
    return "mat4";
}

} // namespace pbrtio::pbrt
