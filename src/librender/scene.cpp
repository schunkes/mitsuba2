#include <mitsuba/core/properties.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/kdtree.h>
#include <mitsuba/render/integrator.h>
#include <enoki/stl.h>

#if defined(MTS_USE_EMBREE)
#  include "scene_embree.inl"
#else
#  include "scene_native.inl"
#endif

#if defined(MTS_USE_OPTIX)
#  include "scene_optix.inl"
#endif

NAMESPACE_BEGIN(mitsuba)

MTS_VARIANT Scene<Float, Spectrum>::Scene(const Properties &props) {
    for (auto &kv : props.objects()) {
        m_children.push_back(kv.second.get());

        Shape *shape           = dynamic_cast<Shape *>(kv.second.get());
        Emitter *emitter       = dynamic_cast<Emitter *>(kv.second.get());
        Sensor *sensor         = dynamic_cast<Sensor *>(kv.second.get());
        Integrator *integrator = dynamic_cast<Integrator *>(kv.second.get());

        if (shape) {
            if (shape->is_emitter())
                m_emitters.push_back(shape->emitter());
            if (shape->is_sensor())
                m_sensors.push_back(shape->sensor());

            m_bbox.expand(shape->bbox());
        } else if (emitter) {
            m_emitters.push_back(emitter);
            if (emitter->is_environment()) {
                if (m_environment)
                    Throw("Only one environment emitter can be specified per scene.");
                m_environment = emitter;
            }
        } else if (sensor) {
            m_sensors.push_back(sensor);
        } else if (integrator) {
            if (m_integrator)
                Throw("Only one integrator can be specified per scene.");
            m_integrator = integrator;
        }
    }

    if (m_sensors.empty()) {
        Log(Warn, "No sensors found! Instantiating a perspective camera..");
        Properties sensor_props("perspective");
        sensor_props.set_float("fov", 45.0f);

        /* Create a perspective camera with a 45 deg. field of view
           and positioned so that it can see the entire scene */
        if (m_bbox.valid()) {
            ScalarPoint3f center = m_bbox.center();
            ScalarVector3f extents = m_bbox.extents();

            ScalarFloat distance =
                hmax(extents) / (2.f * std::tan(45.f * .5f * math::Pi<ScalarFloat> / 180.f));

            sensor_props.set_float("far_clip", hmax(extents) * 5 + distance);
            sensor_props.set_float("near_clip", distance / 100);

            sensor_props.set_float("focus_distance", distance + extents.z() / 2);
            sensor_props.set_transform(
                "to_world", Transform4f::translate(Vector3f(
                                center.x(), center.y(), m_bbox.min.z() - distance)));
        }

        m_sensors.push_back(
            PluginManager::instance()->create_object<Sensor>(sensor_props));
    }

    if (!m_integrator) {
        Log(Warn, "No integrator found! Instantiating a path tracer..");
        m_integrator = PluginManager::instance()->
            create_object<Integrator>(Properties("path"));
    }

    if constexpr (is_cuda_array_v<Float>)
        accel_init_gpu(props);
    else
        accel_init_cpu(props);

    // Precompute a discrete distribution over emitters
    m_emitter_distr = DiscreteDistribution<Float>(m_emitters.size());
    for (size_t i = 0; i < m_emitters.size(); ++i)
        m_emitter_distr.append(1.f);  // Simple uniform distribution for now.
    if (m_emitters.size() > 0)
        m_emitter_distr.normalize();

    // Create emitters' shapes (environment luminaires)
    for (Emitter *emitter: m_emitters)
        emitter->set_scene(this);
}

MTS_VARIANT Scene<Float, Spectrum>::~Scene() {
    if constexpr (is_cuda_array_v<Float>)
        accel_release_gpu();
    else
        accel_release_cpu();
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect(const Ray3f &ray, Mask active) const {
    if constexpr (is_cuda_array_v<Float>)
        return ray_intersect_gpu(ray, active);
    else
        return ray_intersect_cpu(ray, active);
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_naive(const Ray3f &ray, Mask active) const {
#if !defined(MTS_USE_EMBREE)
    if constexpr (!is_cuda_array_v<Float>)
        return ray_intersect_naive_cpu(ray, active);
#endif
    ENOKI_MARK_USED(ray);
    ENOKI_MARK_USED(active);
    NotImplementedError("ray_intersect_naive");
}

MTS_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test(const Ray3f &ray, Mask active) const {
    if constexpr (is_cuda_array_v<Float>)
        return ray_test_gpu(ray, active);
    else
        return ray_test_cpu(ray, active);
}

MTS_VARIANT std::vector<ref<Object>> Scene<Float, Spectrum>::children() { return m_children; }

MTS_VARIANT std::pair<typename Scene<Float, Spectrum>::DirectionSample3f, Spectrum>
Scene<Float, Spectrum>::sample_emitter_direction(const Interaction3f &ref, const Point2f &sample_,
                                                 bool test_visibility, Mask active) const {
    using EmitterPtr = replace_scalar_t<Float, Emitter*>;

    ScopedPhase sp(ProfilerPhase::SampleEmitterDirection);
    Point2f sample(sample_);

    DirectionSample3f ds;
    Spectrum spec = 0.f;

    if (likely(!m_emitters.empty())) {
        // Randomly pick an emitter according to the precomputed emitter distribution
        UInt32 index;
        Float emitter_pdf;
        std::tie(index, emitter_pdf, sample.x()) =
            m_emitter_distr.sample_reuse_pdf(sample.x(), active);
        EmitterPtr emitter = gather<EmitterPtr>(m_emitters.data(), index, active);

        // Sample a direction towards the emitter
        std::tie(ds, spec) = emitter->sample_direction(ref, sample, active);
        active &= neq(ds.pdf, 0.f);

        // Perform a visibility test if requested
        if (test_visibility && any_or<true>(active)) {
            Ray3f ray(ref.p, ds.d, math::Epsilon<Float> * (1.f + hmax(abs(ref.p))),
                      ds.dist * (1.f - math::ShadowEpsilon<Float>), ref.time, ref.wavelengths);
            spec[ray_test(ray, active)] = 0.f;
        }

        // Account for the discrete probability of sampling this emitter
        ds.pdf *= emitter_pdf;
        spec *= rcp(emitter_pdf);
    }

    return { ds, spec };
}

MTS_VARIANT std::string Scene<Float, Spectrum>::to_string() const {
    std::ostringstream oss;
    oss << "Scene[" << std::endl
        << "  children = [" << std::endl;
    for (auto shape : m_children)
        oss << "    " << string::indent(shape->to_string(), 4)
            << "," << std::endl;
    oss << "  ]"<< std::endl
        << "]";
    return oss.str();
}

void librender_nop() { }

MTS_INSTANTIATE_OBJECT(Scene)
NAMESPACE_END(mitsuba)
