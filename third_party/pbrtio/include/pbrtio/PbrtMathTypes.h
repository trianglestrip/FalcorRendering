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

} // namespace pbrtio::pbrt
