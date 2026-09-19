#include <VPGLoader/Model.hpp>

namespace vpgloader {

AABB TransformAABB(const AABB& bounds, const glm::mat4& transform) noexcept
{
    if (!bounds.valid) {
        return {};
    }
    // Transform center and extents; works for rotation, reflection, and shear.
    const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
    const glm::vec3 extent = (bounds.max - bounds.min) * 0.5f;
    const glm::vec3 transformedCenter = glm::vec3(transform * glm::vec4(center, 1.0f));
    const glm::vec3 transformedExtent =
        glm::abs(glm::vec3(transform[0])) * extent.x
        + glm::abs(glm::vec3(transform[1])) * extent.y
        + glm::abs(glm::vec3(transform[2])) * extent.z;
    return {transformedCenter - transformedExtent, transformedCenter + transformedExtent, true};
}

AABB LoadedModel::CalculateBounds(const glm::mat4& worldFromModel) const noexcept
{
    AABB result;
    for (const ModelDrawable& drawable : drawables) {
        ExpandAABB(result, TransformAABB(meshes[drawable.meshIndex].bounds,
                                       worldFromModel * drawable.modelFromMesh));
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
