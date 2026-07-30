#include <VPGLoader/Model.hpp>

#include <cmath>

namespace vpgloader {

glm::mat4 Multiply(const glm::mat4& left, const glm::mat4& right) noexcept
{
    return left * right;
}

glm::vec3 TransformPoint(const glm::mat4& transform,
                         const glm::vec3& point) noexcept
{
    const glm::vec4 transformed = transform * glm::vec4(point, 1.0f);
    glm::vec3 result = glm::vec3(transformed);
    const float w = transformed.w;
    if (w != 0.0f && std::abs(w - 1.0f) > 1.0e-6f) {
        result.x /= w;
        result.y /= w;
        result.z /= w;
    }
    return result;
}

AABB TransformAABB(const AABB& bounds, const glm::mat4& transform) noexcept
{
    if (!bounds.valid) {
        return {};
    }

    const glm::vec3 corners[] = {
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z},
        {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.max.z},
        {bounds.max.x, bounds.max.y, bounds.max.z},
    };

    AABB result;
    for (const glm::vec3& corner : corners) {
        ExpandAABB(result, TransformPoint(transform, corner));
    }
    return result;
}

void ExpandAABB(AABB& bounds, const glm::vec3& point) noexcept
{
    if (!bounds.valid) {
        bounds.min = point;
        bounds.max = point;
        bounds.valid = true;
        return;
    }

    bounds.min = glm::min(bounds.min, point);
    bounds.max = glm::max(bounds.max, point);
}

void ExpandAABB(AABB& bounds, const AABB& other) noexcept
{
    if (!other.valid) {
        return;
    }
    ExpandAABB(bounds, other.min);
    ExpandAABB(bounds, other.max);
}

LoadedModel::LoadedModel() = default;
LoadedModel::~LoadedModel() = default;
LoadedModel::LoadedModel(LoadedModel&&) noexcept = default;
LoadedModel& LoadedModel::operator=(LoadedModel&&) noexcept = default;

} // namespace vpgloader
