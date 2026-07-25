#include <VPGLoader/Model.hpp>

#include <algorithm>
#include <cmath>

namespace vpgloader {

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) noexcept
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (std::size_t inner = 0; inner < 4; ++inner) {
                value += left.values[inner * 4 + row] * right.values[column * 4 + inner];
            }
            result.values[column * 4 + row] = value;
        }
    }
    return result;
}

Float3 TransformPoint(const Matrix4& transform, const Float3& point) noexcept
{
    const auto& m = transform.values;
    Float3 result = {
        m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12],
        m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
        m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14],
    };
    const float w = m[3] * point.x + m[7] * point.y + m[11] * point.z + m[15];
    if (w != 0.0f && std::abs(w - 1.0f) > 1.0e-6f) {
        result.x /= w;
        result.y /= w;
        result.z /= w;
    }
    return result;
}

AABB TransformAABB(const AABB& bounds, const Matrix4& transform) noexcept
{
    if (!bounds.valid) {
        return {};
    }

    const Float3 corners[] = {
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
    for (const Float3& corner : corners) {
        ExpandAABB(result, TransformPoint(transform, corner));
    }
    return result;
}

void ExpandAABB(AABB& bounds, const Float3& point) noexcept
{
    if (!bounds.valid) {
        bounds.min = point;
        bounds.max = point;
        bounds.valid = true;
        return;
    }

    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.min.z = std::min(bounds.min.z, point.z);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
    bounds.max.z = std::max(bounds.max.z, point.z);
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
