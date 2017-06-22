/*******************************************************************************
* Copyright 2017 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_JIT_AVX512_COMMON_1x1_CONVOLUTION_HPP
#define CPU_JIT_AVX512_COMMON_1x1_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "cpu_convolution_pd.hpp"
#include "cpu_engine.hpp"
#include "cpu_reducer.hpp"
#include "jit_avx512_common_1x1_conv_kernel.hpp"
#include "jit_uni_1x1_conv_utils.hpp"
#include "mkldnn_thread.hpp"
#include "utils.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <bool with_relu, impl::data_type_t src_type,
         impl::data_type_t wei_type = src_type,
         impl::data_type_t dst_type = src_type>
struct _jit_avx512_common_1x1_convolution_fwd_t : public cpu_primitive_t {
    // TODO: (Roma) Code duplication duplication! Remove with templates
    //              (maybe...)!
    struct pd_t: public _cpu_convolution_fwd_pd_t<with_relu> {
        pd_t(engine_t *engine,
                const typename pd_t::base_desc_t *adesc,
                const typename pd_t::base_class *hint_fwd_pd)
            : _cpu_convolution_fwd_pd_t<with_relu>(engine, adesc, hint_fwd_pd)
            , jcp_({}), rtus_({}) {}

        DECLARE_COMMON_PD_T(_jit_avx512_common_1x1_convolution_fwd_t);

        virtual status_t init() override {
            using namespace prop_kind;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->set_default_params() == status::success
                && utils::one_of(this->cdesc_().prop_kind, forward_training,
                        forward_inference)
                && utils::implication(
                        this->base_pkind == primitive_kind::convolution_relu,
                        this->cdesc_().prop_kind == forward_inference)
                && this->cdesc_().alg_kind == alg_kind::convolution_direct
                && this->cdesc_().src_desc.data_type == src_type
                && this->cdesc_().weights_desc.data_type == wei_type
                && this->cdesc_().dst_desc.data_type == dst_type
                && utils::implication(this->with_bias(),
                    dst_type == this->cdesc_().bias_desc.data_type);
            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = &this->cdesc_();
            const memory_desc_t *src_d = this->src_pd_.desc();
            rtus_prepare(this, conv_d, src_d, this->dst_pd_.desc());
            return jit_avx512_common_1x1_conv_kernel::init_conf(jcp_,
                    *conv_d, *src_d, *this->weights_pd_.desc(),
                    *this->dst_pd_.desc(), with_relu, this->negative_slope(),
                    omp_get_max_threads(), rtus_.reduce_src_);
        }

        jit_1x1_conv_conf_t jcp_;
        struct reduce_to_unit_stride_t {
            convolution_desc_t conv_d_;
            bool reduce_src_;
        } rtus_;

      protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;
            if (this->src_pd_.desc()->format == any)
                CHECK(this->src_pd_.set_format(nChw16c));
            if (this->dst_pd_.desc()->format == any)
                CHECK(this->dst_pd_.set_format(nChw16c));
            if (this->weights_pd_.desc()->format == any) {
                if (dst_type == data_type::f32 && src_type == data_type::f32
                    && wei_type == data_type::f32)
                        CHECK(this->weights_pd_.set_format(this->with_groups()
                                                ? gOIhw16i16o : OIhw16i16o));
                else if (dst_type == data_type::s32
                    && src_type == data_type::s16
                    && wei_type == data_type::s16)
                        CHECK(this->weights_pd_.set_format(this->with_groups()
                                                ? gOIhw8i16o2i : OIhw8i16o2i));
            }
            if (this->bias_pd_.desc()->format == any)
                CHECK(this->bias_pd_.set_format(x));
            return status::success;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend void init_rtus_driver(conv_t *self);
    _jit_avx512_common_1x1_convolution_fwd_t(const pd_t *pd,
                                          const input_vector &inputs,
                                          const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , kernel_(nullptr), rtus_driver_(nullptr), ws_per_thread_(0)
        , scratch_(nullptr)
    {
        kernel_ = new jit_avx512_common_1x1_conv_kernel(conf_.jcp_);
        init_rtus_driver<avx512_common>(this);
    }
    ~_jit_avx512_common_1x1_convolution_fwd_t() {
        delete kernel_;
        delete rtus_driver_;
        free(scratch_);
    }

    typedef typename prec_traits<src_type>::type src_data_t;
    typedef typename prec_traits<wei_type>::type wei_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;

    virtual void execute(event_t *e) {
        execute_forward();
        e->set_state(event_t::ready);
    }

  private:
    void execute_forward();
    pd_t conf_;
    jit_avx512_common_1x1_conv_kernel *kernel_;
    /* reduction to unit stride */
    rtus_driver_t<avx512_common> *rtus_driver_;
    size_t ws_per_thread_;
    src_data_t *scratch_;
};

using jit_avx512_common_1x1_convolution_fwd_f32_t
        = _jit_avx512_common_1x1_convolution_fwd_t<false, data_type::f32>;
using jit_avx512_common_1x1_convolution_relu_f32_t
        = _jit_avx512_common_1x1_convolution_fwd_t<true, data_type::f32>;
using jit_avx512_common_1x1_convolution_fwd_s16s16s32_t
        = _jit_avx512_common_1x1_convolution_fwd_t<false, data_type::s16,
            data_type::s16, data_type::s32>;

template <impl::data_type_t diff_dst_type,
          impl::data_type_t wei_type = diff_dst_type,
          impl::data_type_t diff_src_type = diff_dst_type>
struct _jit_avx512_common_1x1_convolution_bwd_data_t : public cpu_primitive_t {
    struct pd_t : public cpu_convolution_bwd_data_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_data_pd_t(engine, adesc, hint_fwd_pd)
            , jcp_({}), rtus_({}) {}

        DECLARE_COMMON_PD_T(_jit_avx512_common_1x1_convolution_bwd_data_t);

        virtual status_t init() override {
            using namespace prop_kind;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->set_default_params() == status::success
                && this->desc()->prop_kind == backward_data
                && this->desc()->alg_kind == alg_kind::convolution_direct
                && this->desc()->diff_dst_desc.data_type == diff_dst_type
                && this->desc()->weights_desc.data_type == wei_type
                && this->desc()->diff_src_desc.data_type == diff_src_type;
            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = this->desc();
            const memory_desc_t *diff_src_d = this->diff_src_pd_.desc();
            rtus_prepare(this, conv_d, diff_src_d, this->diff_dst_pd_.desc());
            return jit_avx512_common_1x1_conv_kernel::init_conf(jcp_,
                            *conv_d, *diff_src_d, *this->weights_pd_.desc(),
                            *this->diff_dst_pd_.desc(), omp_get_max_threads(),
                            rtus_.reduce_src_);
        }

        // TODO (Roma): structs conf header cleanup
        jit_1x1_conv_conf_t jcp_;
        struct reduce_to_unit_stride_t {
            convolution_desc_t conv_d_;
            bool reduce_src_;
        } rtus_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;

            if (this->diff_src_pd_.desc()->format == any)
                CHECK(this->diff_src_pd_.set_format(nChw16c));
            if (this->diff_dst_pd_.desc()->format == any)
                CHECK(this->diff_dst_pd_.set_format(nChw16c));
            if (this->weights_pd_.desc()->format == any) {
                if (diff_dst_type == data_type::f32
                    && diff_src_type == data_type::f32
                    && wei_type == data_type::f32)
                        CHECK(this->weights_pd_.set_format(this->with_groups()
                                                ? gOIhw16o16i : OIhw16o16i));
                else if (diff_dst_type == data_type::s16
                    && diff_src_type == data_type::s32
                    && wei_type == data_type::s16)
                        CHECK(this->weights_pd_.set_format(this->with_groups()
                                                ? gOIhw8o16i2o : OIhw8o16i2o));
            }

            return status::success;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend void init_rtus_driver(conv_t *self);
    _jit_avx512_common_1x1_convolution_bwd_data_t(const pd_t *pd,
                                              const input_vector &inputs,
                                              const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , kernel_(nullptr), rtus_driver_(nullptr), ws_per_thread_(0)
        , scratch_(nullptr)
    {
        kernel_ = new jit_avx512_common_1x1_conv_kernel(conf_.jcp_);
        init_rtus_driver<avx512_common>(this);
    }
    ~_jit_avx512_common_1x1_convolution_bwd_data_t()
    {
        delete kernel_;
        delete rtus_driver_;
        free(scratch_);
    }

    typedef typename prec_traits<diff_dst_type>::type diff_dst_data_t;
    typedef typename prec_traits<wei_type>::type wei_data_t;
    typedef typename prec_traits<diff_src_type>::type diff_src_data_t;

    virtual void execute(event_t *e) {
        switch (conf_.desc()->prop_kind) {
        case prop_kind::backward_data:
            execute_backward_data();
            break;
        default:
            assert(!"invalid prop_kind");
        }
        e->set_state(event_t::ready);
    }

  private:
    void execute_backward_data();
    pd_t conf_;
    jit_avx512_common_1x1_conv_kernel *kernel_;
    /* reduction to unit stride */
    rtus_driver_t<avx512_common> *rtus_driver_;
    size_t ws_per_thread_;
    diff_src_data_t *scratch_;
};

using jit_avx512_common_1x1_convolution_bwd_data_f32_t
        = _jit_avx512_common_1x1_convolution_bwd_data_t<data_type::f32>;
using jit_avx512_common_1x1_convolution_bwd_data_s16s16s32_t
        = _jit_avx512_common_1x1_convolution_bwd_data_t<data_type::s16,
            data_type::s16, data_type::s32>;

struct jit_avx512_common_1x1_convolution_bwd_weights_t : public cpu_primitive_t {
    struct pd_t : public cpu_convolution_bwd_weights_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_weights_pd_t(engine, adesc, hint_fwd_pd)
            , jcp_({}), rtus_({}) {}

        DECLARE_COMMON_PD_T(jit_avx512_common_1x1_convolution_bwd_weights_t);

        virtual status_t init() override {
            using namespace prop_kind;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->set_default_params() == status::success
                && this->desc()->prop_kind == backward_weights
                && this->desc()->alg_kind == alg_kind::convolution_direct
                && utils::everyone_is(data_type::f32,
                        this->desc()->src_desc.data_type,
                        this->desc()->diff_weights_desc.data_type,
                        this->desc()->diff_dst_desc.data_type)
                && utils::implication(this->with_bias(),
                        data_type::f32 == desc()->diff_bias_desc.data_type);
            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = this->desc();
            const memory_desc_t *src_d = this->src_pd_.desc();
            rtus_prepare(this, conv_d, src_d, this->diff_dst_pd_.desc());
            return jit_avx512_common_1x1_conv_kernel::init_conf(jcp_,
                            *conv_d, *src_d, *this->diff_weights_pd_.desc(),
                            *this->diff_dst_pd_.desc(), omp_get_max_threads(),
                            rtus_.reduce_src_);
        }

        // TODO (Roma): structs conf header cleanup
        jit_1x1_conv_conf_t jcp_;

        struct reduce_to_unit_stride_t {
            convolution_desc_t conv_d_;
            bool reduce_src_;
        } rtus_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;

            if (this->src_pd_.desc()->format == any)
                CHECK(this->src_pd_.set_format(nChw16c));
            if (this->diff_dst_pd_.desc()->format == any)
                CHECK(this->diff_dst_pd_.set_format(nChw16c));
            if (this->diff_weights_pd_.desc()->format == any)
                CHECK(this->diff_weights_pd_.set_format(this->with_groups()
                                                ? gOIhw16i16o : OIhw16i16o));
            if (this->diff_bias_pd_.desc()->format == any)
                CHECK(this->diff_bias_pd_.set_format(x));
            return status::success;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend void init_rtus_driver(conv_t *self);
    jit_avx512_common_1x1_convolution_bwd_weights_t(const pd_t *pd,
                                                 const input_vector &inputs,
                                                 const output_vector &outputs);
    ~jit_avx512_common_1x1_convolution_bwd_weights_t() {
        delete kernel_;
        delete rtus_driver_;
        free(scratch_);
    }

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual void execute(event_t *e) {
        switch (conf_.desc()->prop_kind) {
        case prop_kind::backward_weights:
            execute_backward_weights();
            break;
        default:
            assert(!"invalid prop_kind");
        }
        e->set_state(event_t::ready);
    }

  private:
    void execute_backward_weights();
    pd_t conf_;
    jit_avx512_common_1x1_conv_kernel *kernel_;
    cpu_reducer_2d_t<data_type::f32> *reducer_weights_;
    cpu_reducer_t<data_type::f32> *reducer_bias_;

    /* reduction to unit stride */
    rtus_driver_t<avx512_common> *rtus_driver_;
    size_t ws_per_thread_;
    data_t *scratch_;
};

}
}
}

#endif
