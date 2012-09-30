
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2012 Francois Beaune, Jupiter Jazz Limited
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "diagnosticsurfaceshader.h"

// appleseed.renderer headers.
#include "renderer/kernel/shading/ambientocclusion.h"
#include "renderer/kernel/shading/shadingcontext.h"
#include "renderer/kernel/shading/shadingpoint.h"
#include "renderer/kernel/shading/shadingresult.h"
#include "renderer/modeling/camera/camera.h"
#include "renderer/modeling/input/inputarray.h"
#include "renderer/modeling/input/source.h"
#include "renderer/modeling/material/material.h"
#include "renderer/modeling/scene/scene.h"
#include "renderer/utility/transformsequence.h"

// appleseed.foundation headers.
#include "foundation/image/colorspace.h"
#include "foundation/math/distance.h"
#include "foundation/math/frustum.h"
#include "foundation/math/hash.h"
#include "foundation/math/minmax.h"
#include "foundation/math/scalar.h"
#include "foundation/platform/types.h"
#include "foundation/utility/containers/specializedarrays.h"

// Standard headers.
#include <cassert>
#include <cmath>

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    const char* Model = "diagnostic_surface_shader";
}

const KeyValuePair<const char*, DiagnosticSurfaceShader::ShadingMode>
    DiagnosticSurfaceShader::ShadingModeValues[] =
{
    { "coverage",                   Coverage },
    { "barycentric",                Barycentric },
    { "uv",                         UV },
    { "tangent",                    Tangent },
    { "bitangent",                  Bitangent },
    { "geometric_normal",           GeometricNormal },
    { "shading_normal",             ShadingNormal },
    { "original_shading_normal",    OriginalShadingNormal },
    { "sides",                      Sides },
    { "depth",                      Depth },
    { "wireframe" ,                 Wireframe },
    { "ambient_occlusion",          AmbientOcclusion },
    { "assembly_instances",         AssemblyInstances },
    { "object_instances",           ObjectInstances },
    { "regions",                    Regions },
    { "triangles",                  Triangles },
    { "materials",                  Materials }
};

const KeyValuePair<const char*, const char*> DiagnosticSurfaceShader::ShadingModeNames[] =
{
    { "coverage",                   "Coverage" },
    { "barycentric",                "Barycentric Coordinates" },
    { "uv",                         "UV Coordinates" },
    { "tangent",                    "Tangents" },
    { "bitangent",                  "Bitangents" },
    { "geometric_normal",           "Geometric Normals" },
    { "shading_normal",             "Shading Normals" },
    { "original_shading_normal",    "Original Shading Normals" },
    { "sides",                      "Sides" },
    { "depth",                      "Depth" },
    { "wireframe" ,                 "Wireframe" },
    { "ambient_occlusion",          "Ambient Occlusion" },
    { "assembly_instances",         "Assembly Instances" },
    { "object_instances",           "Object Instances" },
    { "regions",                    "Regions" },
    { "triangles",                  "Triangles" },
    { "materials",                  "Materials" }
};

DiagnosticSurfaceShader::DiagnosticSurfaceShader(
    const char*             name,
    const ParamArray&       params)
  : SurfaceShader(name, params)
{
    extract_parameters();
}

void DiagnosticSurfaceShader::release()
{
    delete this;
}

const char* DiagnosticSurfaceShader::get_model() const
{
    return Model;
}

namespace
{
    // Like foundation::wrap() defined in foundation/math/scalar.h but for [0,1] instead of [0,1).
    template <typename T>
    inline T wrap1(const T x)
    {
        if (x < T(0.0) || x > T(1.0))
        {
            const T y = fmod(x, T(1.0));
            return y < T(0.0) ? y + T(1.0) : y;
        }
        else return x;
    }

    // Compute a color from a given 2D vector.
    inline Color3f vector2_to_color(const Vector2d& vec)
    {
        const float u = wrap1(static_cast<float>(vec[0]));
        const float v = wrap1(static_cast<float>(vec[1]));
        const float w = wrap1(1.0f - u - v);
        return Color3f(u, v, w);
    }

    // Compute a color from a given unit-length 3D vector.
    inline Color3f vector3_to_color(const Vector3d& vec)
    {
        assert(is_normalized(vec));

        return Color3f(
            static_cast<float>((vec[0] + 1.0) * 0.5),
            static_cast<float>((vec[1] + 1.0) * 0.5),
            static_cast<float>((vec[2] + 1.0) * 0.5));
    }

    // Compute a color from a given integer.
    template <typename T>
    inline Color3f integer_to_color(const T i)
    {
        const uint32 u = static_cast<uint32>(i);    // keep the low 32 bits
        const uint32 x = hashint32(u);
        const uint32 y = hashint32(u + 1);
        const uint32 z = hashint32(u + 2);

        return Color3f(
            static_cast<float>(x) * (1.0f / 4294967295.0f),
            static_cast<float>(y) * (1.0f / 4294967295.0f),
            static_cast<float>(z) * (1.0f / 4294967295.0f));
    }
}

void DiagnosticSurfaceShader::evaluate(
    SamplingContext&        sampling_context,
    const ShadingContext&   shading_context,
    const ShadingPoint&     shading_point,
    ShadingResult&          shading_result) const
{
    switch (m_shading_mode)
    {
      case Coverage:
        shading_result.set_to_linear_rgb(Color3f(1.0f));
        break;

      case Barycentric:
        shading_result.set_to_linear_rgb(
            vector2_to_color(shading_point.get_bary()));
        break;

      case UV:
        shading_result.set_to_linear_rgb(
            vector2_to_color(shading_point.get_uv(0)));
        break;

      case Tangent:
        shading_result.set_to_linear_rgb(
            vector3_to_color(shading_point.get_dpdu(0)));
        break;

      case Bitangent:
        shading_result.set_to_linear_rgb(
            vector3_to_color(shading_point.get_dpdv(0)));
        break;

      case GeometricNormal:
        shading_result.set_to_linear_rgb(
            vector3_to_color(shading_point.get_geometric_normal()));
        break;

      case ShadingNormal:
        shading_result.set_to_linear_rgb(
            vector3_to_color(shading_point.get_shading_normal()));
        break;

      case OriginalShadingNormal:
        shading_result.set_to_linear_rgb(
            vector3_to_color(shading_point.get_original_shading_normal()));
        break;

      case Sides:
        shading_result.set_to_linear_rgb(
            shading_point.get_side() == ObjectInstance::FrontSide
                ? Color3f(0.0f, 0.0f, 1.0f)
                : Color3f(1.0f, 0.0f, 0.0f));
        break;

      case Depth:
        shading_result.set_to_linear_rgb(
            Color3f(static_cast<float>(shading_point.get_distance())));
        break;

      case Wireframe:
        {
            // Film space thickness of the wires.
            const double SquareWireThickness = square(0.0005);

            // Initialize the shading result to the background color.
            shading_result.set_to_linear_rgba(Color4f(0.0f, 0.0f, 0.8f, 0.5f));

            // Retrieve the camera.
            const Scene& scene = shading_point.get_scene();
            const Camera& camera = *scene.get_camera();
            const double time = shading_point.get_ray().m_time;
            const Transformd camera_transform = camera.transform_sequence().evaluate(time);
            const Pyramid3d& view_pyramid = camera.get_view_pyramid();

            // Compute the film space coordinates of the intersection point.
            const Vector3d& point = shading_point.get_point();
            const Vector3d point_cs = camera_transform.point_to_local(point);
            const Vector2d point_fs = camera.project(point_cs);

            // Compute the camera space coordinates of the triangle vertices.
            Vector3d v_cs[3];
            v_cs[0] = camera_transform.point_to_local(shading_point.get_vertex(0));
            v_cs[1] = camera_transform.point_to_local(shading_point.get_vertex(1));
            v_cs[2] = camera_transform.point_to_local(shading_point.get_vertex(2));

            // Loop over the triangle edges.
            for (size_t i = 0; i < 3; ++i)
            {
                // Compute the end points of this edge.
                const size_t j = (i + 1) % 3;
                Vector3d vi_cs = v_cs[i];
                Vector3d vj_cs = v_cs[j];

                // Clip the edge against the view pyramid.
                if (!view_pyramid.clip(vi_cs, vj_cs))
                    continue;

                // Transform the edge to film space.
                const Vector2d vi_fs = camera.project(vi_cs);
                const Vector2d vj_fs = camera.project(vj_cs);

                // Compute the film space distance from the intersection point to the edge.
                const double d = square_distance_point_segment(point_fs, vi_fs, vj_fs);

                if (d < SquareWireThickness)
                {
                    shading_result.set_to_linear_rgba(Color4f(1.0f));
                    break;
                }
            }
        }
        break;

      case AmbientOcclusion:
        {
            // Compute the occlusion.
            const double occlusion =
                compute_ambient_occlusion(
                    sampling_context,
                    sample_hemisphere_uniform<double>,
                    shading_context.get_intersector(),
                    shading_point.get_point(),
                    shading_point.get_geometric_normal(),
                    shading_point.get_shading_basis(),
                    shading_point.get_ray().m_time,
                    m_ao_max_distance,
                    m_ao_samples,
                    &shading_point);

            // Return a gray scale value proportional to the accessibility.
            const float accessibility = static_cast<float>(1.0 - occlusion);
            shading_result.set_to_linear_rgb(Color3f(accessibility));
        }
        break;

      case AssemblyInstances:
        shading_result.set_to_linear_rgb(
            integer_to_color(shading_point.get_assembly_instance().get_uid()));
        break;

      case ObjectInstances:
        shading_result.set_to_linear_rgb(
            integer_to_color(shading_point.get_object_instance().get_uid()));
        break;

      case Regions:
        {
            const uint32 h =
                mix32(
                    static_cast<uint32>(shading_point.get_object_instance().get_uid()),
                    static_cast<uint32>(shading_point.get_region_index()));
            shading_result.set_to_linear_rgb(integer_to_color(h));
        }
        break;

      case Triangles:
        {
            const uint32 h =
                mix32(
                    static_cast<uint32>(shading_point.get_object_instance().get_uid()),
                    static_cast<uint32>(shading_point.get_region_index()),
                    static_cast<uint32>(shading_point.get_triangle_index()));
            shading_result.set_to_linear_rgb(integer_to_color(h));
        }
        break;

      case Materials:
        {
            const Material* material = shading_point.get_material();
            if (material)
                shading_result.set_to_linear_rgb(integer_to_color(material->get_uid()));
            else shading_result.set_to_solid_pink();
        }
        break;

      default:
        assert(false);
        shading_result.clear();
        break;
    }
}

void DiagnosticSurfaceShader::extract_parameters()
{
    // Retrieve shading mode.
    const string mode_string = m_params.get_required<string>("mode", "coverage");
    const KeyValuePair<const char*, ShadingMode>* mode_pair =
        lookup_kvpair_array(ShadingModeValues, ShadingModeCount, mode_string);
    if (mode_pair)
        m_shading_mode = mode_pair->m_value;
    else
    {
        RENDERER_LOG_ERROR(
            "invalid shading mode \"%s\", using default value \"coverage\".",
            mode_string.c_str());
        m_shading_mode = Coverage;
    }

    // Retrieve ambient occlusion parameters.
    if (m_shading_mode == AmbientOcclusion)
    {
        const ParamArray& ao_params = m_params.child("ambient_occlusion");
        m_ao_max_distance = ao_params.get_optional<double>("max_distance", 1.0);
        m_ao_samples = ao_params.get_optional<size_t>("samples", 16);
    }
}


//
// DiagnosticSurfaceShaderFactory class implementation.
//

const char* DiagnosticSurfaceShaderFactory::get_model() const
{
    return Model;
}

const char* DiagnosticSurfaceShaderFactory::get_human_readable_model() const
{
    return "Diagnostics";
}

DictionaryArray DiagnosticSurfaceShaderFactory::get_widget_definitions() const
{
    Dictionary model_items;

    for (int i = 0; i < DiagnosticSurfaceShader::ShadingModeCount; ++i)
    {
        const char* shading_mode_value = DiagnosticSurfaceShader::ShadingModeNames[i].m_key;
        const char* shading_mode_name = DiagnosticSurfaceShader::ShadingModeNames[i].m_value;
        model_items.insert(shading_mode_name, shading_mode_value);
    }

    DictionaryArray definitions;

    definitions.push_back(
        Dictionary()
            .insert("name", "mode")
            .insert("label", "Mode")
            .insert("widget", "dropdown_list")
            .insert("dropdown_items", model_items)
            .insert("use", "required")
            .insert("default", "coverage")
            .insert("on_change", "rebuild_form"));

    return definitions;
}

auto_release_ptr<SurfaceShader> DiagnosticSurfaceShaderFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return
        auto_release_ptr<SurfaceShader>(
            new DiagnosticSurfaceShader(name, params));
}

}   // namespace renderer
