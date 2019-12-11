#include <thread>

#include <enoki/morton.h>
#include <mitsuba/core/profiler.h>
#include <mitsuba/core/progress.h>
#include <mitsuba/core/spectrum.h>
#include <mitsuba/core/timer.h>
#include <mitsuba/core/util.h>
#include <mitsuba/core/warp.h>
#include <mitsuba/render/film.h>
#include <mitsuba/render/integrator.h>
#include <mitsuba/render/sampler.h>
#include <mitsuba/render/sensor.h>
#include <mitsuba/render/spiral.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

NAMESPACE_BEGIN(mitsuba)

// -----------------------------------------------------------------------------

template <typename Float, typename Spectrum>
SamplingIntegrator<Float, Spectrum>::SamplingIntegrator(const Properties &props)
    : Base(props) {
    m_block_size = (uint32_t) props.size_("block_size", MTS_BLOCK_SIZE);
    uint32_t block_size = math::round_to_power_of_two(m_block_size);
    if (block_size != m_block_size) {
        m_block_size = block_size;
        Log(Warn, "Setting block size from %i to next higher power of two: %i", block_size,
            m_block_size);
    }

    m_samples_per_pass = (uint32_t) props.size_("samples_per_pass", (size_t) -1);
    m_timeout = props.float_("timeout", -1.f);
}

template <typename Float, typename Spectrum>
SamplingIntegrator<Float, Spectrum>::~SamplingIntegrator() { }

template <typename Float, typename Spectrum>
void SamplingIntegrator<Float, Spectrum>::cancel() {
    m_stop = true;
}

template <typename Float, typename Spectrum>
bool SamplingIntegrator<Float, Spectrum>::render(Scene *scene, Sensor *sensor) {
    ScopedPhase sp(ProfilerPhase::Render);
    m_stop = false;

    ref<Film> film = sensor->film();
    ScalarVector2i film_size = film->crop_size();

    size_t n_threads        = __global_thread_count;
    size_t total_spp        = sensor->sampler()->sample_count();
    size_t samples_per_pass = (m_samples_per_pass == (size_t) -1)
                               ? total_spp : std::min((size_t) m_samples_per_pass, total_spp);
    if ((total_spp % samples_per_pass) != 0)
        Throw("sample_count (%d) must be a multiple of samples_per_pass (%d).",
              total_spp, samples_per_pass);

    size_t n_passes = ceil(total_spp / (ScalarFloat) samples_per_pass);
    film->clear();

    if constexpr (!is_cuda_array_v<Float>) {
        Log(Info, "Starting render job (%ix%i, %i sample%s,%s %i thread%s)",
            film_size.x(), film_size.y(),
            total_spp, total_spp == 1 ? "" : "s",
            n_passes > 1 ? tfm::format(" %d passes,", n_passes) : "",
            n_threads, n_threads == 1 ? "" : "s");
        if (m_timeout > 0.f)
            Log(Info, "Timeout specified: %.2f seconds.", m_timeout);

        Spiral spiral(film, m_block_size, n_passes);

        ThreadEnvironment env;
        ref<ProgressReporter> progress = new ProgressReporter("Rendering");
        tbb::spin_mutex mutex;
        size_t blocks_done = 0;
        // Total number of blocks to be handled, including multiple passes.
        ScalarFloat total_blocks = spiral.block_count() * n_passes;

        m_render_timer.reset();
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, total_blocks, 1),
            [&](const tbb::blocked_range<size_t> &range) {
                ScopedSetThreadEnvironment set_env(env);
                ref<Sampler> sampler = sensor->sampler()->clone();
                ref<ImageBlock> block =
                    new ImageBlock(Bitmap::PixelFormat::XYZAW, ScalarVector2i(m_block_size),
                                film->reconstruction_filter(), 0, true);
                scoped_flush_denormals flush_denormals(true);

                // For each block
                for (auto i = range.begin(); i != range.end() && !should_stop(); ++i) {
                    auto [offset, size] = spiral.next_block();
                    if (hprod(size) == 0)
                        Throw("Internal error -- generated empty image block!");
                    if (size != block->size())
                        block = new ImageBlock(Bitmap::PixelFormat::XYZAW, size,
                                            film->reconstruction_filter(), 0, true);
                    block->set_offset(offset);

                    // Ensure that the sample generation is fully deterministic
                    size_t seed = (size_t) offset.x() +
                                  (size_t) offset.y() * (size_t) film_size.x();
                    if (n_passes > 1)
                        seed += i * hprod(film_size);
                    sampler->seed(seed);

                    render_block(scene, sensor, sampler, block, samples_per_pass);

                    film->put(block);

                    /* locked */ {
                        tbb::spin_mutex::scoped_lock lock(mutex);
                        blocks_done++;
                        progress->update(blocks_done / (ScalarFloat) total_blocks);
                    }
                }
            }
        );
    } else {
        for (size_t i = 0; i < n_passes; i++) {
            ScalarUInt32 total_sample_count = hprod(film_size) * samples_per_pass;

            Mask active(true);
            set_slices(active, total_sample_count);

            ref<Sampler> sampler = sensor->sampler();
            sampler->seed(arange<UInt64>(total_sample_count));

            ScalarFloat diff_scale_factor = rsqrt((ScalarFloat) sampler->sample_count());

            ref<ImageBlock> block = new ImageBlock(Bitmap::PixelFormat::XYZAW, film_size,
                                                   film->reconstruction_filter(), 0, true);

            UInt32 idx = arange<UInt32>(total_sample_count) / UInt32(samples_per_pass);
            Vector2f pos = Vector2f(Float(idx % int(film_size[0])),
                                    Float(idx / int(film_size[0])));

            render_sample(scene, sensor, sampler, block, pos, diff_scale_factor, active);

            film->put(block);
        }
    }

    if (!m_stop)
        Log(Info, "Rendering finished. (took %s)",
            util::time_string(m_render_timer.value(), true));

    return !m_stop;
}

template <typename Float, typename Spectrum>
void SamplingIntegrator<Float, Spectrum>::render_block(const Scene *scene,
                                                       const Sensor *sensor,
                                                       Sampler *sampler,
                                                       ImageBlock *block,
                                                       size_t sample_count_) const {
    block->clear();
    uint32_t pixel_count  = (uint32_t)(m_block_size * m_block_size),
             sample_count = (uint32_t)(sample_count_ == (size_t) -1
                                           ? sampler->sample_count()
                                           : sample_count_);

    ScalarFloat diff_scale_factor = rsqrt((ScalarFloat) sampler->sample_count());

    if constexpr (!is_array_v<Float>) {
        for (uint32_t i = 0; i < pixel_count && !should_stop(); ++i) {
            ScalarPoint2u pos = enoki::morton_decode<ScalarPoint2u>(i);
            if (any(pos >= block->size()))
                continue;

            pos += block->offset();
            for (uint32_t j = 0; j < sample_count && !should_stop(); ++j) {
                render_sample(scene, sensor, sampler, block, pos,
                              diff_scale_factor);
            }
        }
    } else if constexpr (is_array_v<Float> && !is_cuda_array_v<Float>) {
        for (auto [index, active] : range<UInt32>(pixel_count * sample_count)) {
            Point2u pos = enoki::morton_decode<Point2u>(index / UInt32(sample_count));
            active &= !any(pos >= block->size());
            pos += block->offset();
            render_sample(scene, sensor, sampler, block, pos, diff_scale_factor, active);
        }
    } else {
        Throw("Not implemented for CUDA arrays.");
    }
}

template <typename Float, typename Spectrum>
void SamplingIntegrator<Float, Spectrum>::render_sample(const Scene *scene,
                                                        const Sensor *sensor,
                                                        Sampler *sampler,
                                                        ImageBlock *block,
                                                        const Vector2f &pos,
                                                        ScalarFloat diff_scale_factor,
                                                        Mask active) const {
    Vector2f position_sample = pos + sampler->next_2d(active);

    Point2f aperture_sample(.5f);
    if (sensor->needs_aperture_sample())
        aperture_sample = sampler->next_2d(active);

    Float time = sensor->shutter_open();
    if (sensor->shutter_open_time() > 0)
        time += sampler->next_1d(active) * sensor->shutter_open_time();

    Float wavelength_sample = sampler->next_1d(active);

    auto adjusted_position =
        (position_sample - sensor->film()->crop_offset()) / sensor->film()->crop_size();
    auto [ray, ray_weight] = sensor->sample_ray_differential(
        time, wavelength_sample, adjusted_position, aperture_sample);

    ray.scale_differential(diff_scale_factor);

    Spectrum result;
    Float alpha(1);

    {   /* Integrator::eval */
        ScopedPhase sp(ProfilerPhase::SamplingIntegratorEval);
        std::tie(result, active) = sample(scene, sampler, ray, active);
    }

    {   /* ImageBlock::put */
        ScopedPhase sp(ProfilerPhase::ImageBlockPut);
        block->put(position_sample, ray.wavelengths,
                    ray_weight * result, alpha);
    }
}

template <typename Float, typename Spectrum>
std::pair<Spectrum, typename SamplingIntegrator<Float, Spectrum>::Mask>
SamplingIntegrator<Float, Spectrum>::sample(const Scene * /*scene*/, Sampler * /*sampler*/,
                                            const RayDifferential3f & /*ray*/,
                                            Mask /*active*/) const {
    NotImplementedError("sample");
}

// -----------------------------------------------------------------------------

template <typename Float, typename Spectrum>
MonteCarloIntegrator<Float, Spectrum>::MonteCarloIntegrator(const Properties &props)
    : Base(props) {
    /// Depth to begin using russian roulette
    m_rr_depth = props.int_("rr_depth", 5);
    if (m_rr_depth <= 0)
        Throw("\"rr_depth\" must be set to a value greater than zero!");

    /*  Longest visualized path depth (\c -1 = infinite). A value of \c 1 will
        visualize only directly visible light sources. \c 2 will lead to
        single-bounce (direct-only) illumination, and so on. */
    m_max_depth = props.int_("max_depth", -1);
    if (m_max_depth < 0 && m_max_depth != -1)
        Throw("\"max_depth\" must be set to -1 (infinite) or a value >= 0");
}

template <typename Float, typename Spectrum>
MonteCarloIntegrator<Float, Spectrum>::~MonteCarloIntegrator() { }


MTS_INSTANTIATE_CLASS(Integrator)
MTS_INSTANTIATE_CLASS(SamplingIntegrator)
MTS_INSTANTIATE_CLASS(MonteCarloIntegrator)
NAMESPACE_END(mitsuba)
