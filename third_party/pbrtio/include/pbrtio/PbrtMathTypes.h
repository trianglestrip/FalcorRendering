/*
 * Math aliases used by the PBRT parser/scene loader.
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace pbrtio::pbrt {

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using float4x4 = glm::mat4;
using mat4f = glm::mat4;

using glm::cross;
using glm::dot;
using glm::inverse;
using glm::length;
using glm::normalize;

inline float3 min(float3 a, float3 b) {
    return glm::min(a, b);
}

inline float3 max(float3 a, float3 b) {
    return glm::max(a, b);
}

inline float3 xyz(const float4& v) {
    return float3(v);
}

} // namespace pbrtio::pbrt
