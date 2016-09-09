/*
 * Copyright 2011-2016 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Evaluate the BSDF from wi to wo.
 * Evaluation is split into the analytical single-scattering BSDF and the multi-scattering BSDF,
 * which is evaluated stochastically through a random walk. At each bounce (except for the first one),
 * the amount of reflection from here towards wo is evaluated before bouncing again.
 *
 * Because of the random walk, the evaluation is not deterministic, but its expected value is equal to
 * the correct BSDF, which is enough for Monte-Carlo rendering. The PDF also can't be determined
 * analytically, so the single-scattering PDF plus a diffuse term to account for the multi-scattered
 * energy is used. In combination with MIS, that is enough to produce an unbiased result, although
 * the balance heuristic isn't necessarily optimal anymore.
 */
ccl_device_inline float3 MF_FUNCTION_FULL_NAME(mf_eval)(
        float3 wi,
        float3 wo,
        const bool wo_outside,
		const float3 color,
		const float3 cspec0,
        const float alpha_x,
        const float alpha_y,
         ccl_addr_space uint *lcg_state
#ifdef MF_MULTI_GLASS
        , const float eta
		, bool use_fresnel = false
		, bool initial_outside = true
#elif defined(MF_MULTI_GLOSSY)
		 , float3 *n, float3 *k
		 , const float eta = 1.0f
		 , bool use_fresnel = false
#endif
)
{
	/* Evaluating for a shallower incoming direction produces less noise, and the properties of the BSDF guarantee reciprocity. */
	bool swapped = false;
#ifdef MF_MULTI_GLASS
	if(wi.z*wo.z < 0.0f) {
		/* Glass transmission is a special case and requires the directions to change hemisphere. */
		if(-wo.z < wi.z) {
			swapped = true;
			float3 tmp = -wo;
			wo = -wi;
			wi = tmp;
		}
	}
	else
#endif
	if(wo.z < wi.z) {
		swapped = true;
		float3 tmp = wo;
		wo = wi;
		wi = tmp;
	}

	if(wi.z < 1e-5f || (wo.z < 1e-5f && wo_outside) || (wo.z > -1e-5f && !wo_outside))
		return make_float3(0.0f, 0.0f, 0.0f);

	const float2 alpha = make_float2(alpha_x, alpha_y);

	float lambda_r = mf_lambda(-wi, alpha);
	float shadowing_lambda = mf_lambda(wo_outside? wo: -wo, alpha);

	/* Analytically compute single scattering for lower noise. */
	float3 eval;
#ifdef MF_MULTI_GLASS
	eval = mf_eval_phase_glass(-wi, lambda_r, wo, wo_outside, alpha, eta);
	if(wo_outside)
		eval *= -lambda_r / (shadowing_lambda - lambda_r);
	else
		eval *= -lambda_r * beta(-lambda_r, shadowing_lambda+1.0f);

	float3 eval2;
	float3 t_color = cspec0;
	float3 throughput2 = make_float3(1.0f, 1.0f, 1.0f);
	float F0 = fresnel_dielectric_cos(1.0f, eta);
	float F0_norm = 1.0f / (1.0f - F0);
	if (use_fresnel && initial_outside) {
		float FH = (fresnel_dielectric_cos(dot(wi, normalize(wi + wo)), eta) - F0) * F0_norm; //schlick_fresnel(dot(wi, normalize(wi + wo))); //
		throughput2 = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

		eval2 = throughput2 * eval;
	}
#elif defined(MF_MULTI_DIFFUSE)
	/* Diffuse has no special closed form for the single scattering bounce */
	eval = make_float3(0.0f, 0.0f, 0.0f);
#else /* MF_MULTI_GLOSSY */
	const float3 wh = normalize(wi+wo);
	const float G2 = 1.0f / (1.0f - (lambda_r + 1.0f) + shadowing_lambda);
	float val = G2 * 0.25f / wi.z;
	if(alpha.x == alpha.y)
		val *= D_ggx(wh, alpha.x);
	else
		val *= D_ggx_aniso(wh, alpha);
	if(n && k) {
		eval = fresnel_conductor(dot(wh, wi), *n, *k) * val;
	}
	else {
		eval = make_float3(val, val, val);
	}

	float3 eval2;
	float3 t_color = cspec0;
	float3 throughput2 = make_float3(1.0f, 1.0f, 1.0f);
	float F0 = fresnel_dielectric_cos(1.0f, eta);
	float F0_norm = 1.0f / (1.0f - F0);
	if (use_fresnel) {
		float FH = (fresnel_dielectric_cos(dot(wi, normalize(wi + wo)), eta) - F0) * F0_norm; //schlick_fresnel(dot(wi, normalize(wi + wo))); //
		throughput2 = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

		eval2 = throughput2 * val;
	}
#endif

	float3 wr = -wi;
	float hr = 1.0f;
	float C1_r = 1.0f;
	float G1_r = 0.0f;
	bool outside = true;
	float3 throughput = make_float3(1.0f, 1.0f, 1.0f);

	for(int order = 0; order < 10; order++) {
		/* Sample microfacet height and normal */
		if(!mf_sample_height(wr, &hr, &C1_r, &G1_r, &lambda_r, lcg_step_float_addrspace(lcg_state)))
			break;
		float3 wm = mf_sample_vndf(-wr, alpha, make_float2(lcg_step_float_addrspace(lcg_state),
		                                                   lcg_step_float_addrspace(lcg_state)));

#ifdef MF_MULTI_DIFFUSE
		if(order == 0) {
			/* Compute single-scattering for diffuse. */
			const float G2_G1 = -lambda_r / (shadowing_lambda - lambda_r);
			eval += throughput * G2_G1 * mf_eval_phase_diffuse(wo, wm);
		}
#endif
#ifdef MF_MULTI_GLASS
		if (order == 0 && use_fresnel) {
			/* Evaluate amount of scattering towards wo on this microfacet. */
			float3 phase;
			if (outside)
				phase = mf_eval_phase_glass(wr, lambda_r, wo, wo_outside, alpha, eta);
			else
				phase = mf_eval_phase_glass(wr, lambda_r, -wo, !wo_outside, alpha, 1.0f / eta);

			eval2 = throughput2 * phase * mf_G1(wo_outside ? wo : -wo, mf_C1((outside == wo_outside) ? hr : -hr), shadowing_lambda);
		}
#endif
		if(order > 0) {
			/* Evaluate amount of scattering towards wo on this microfacet. */
			float3 phase;
#ifdef MF_MULTI_GLASS
			if(outside)
				phase = mf_eval_phase_glass(wr, lambda_r,  wo,  wo_outside, alpha, eta);
			else
				phase = mf_eval_phase_glass(wr, lambda_r, -wo, !wo_outside, alpha, 1.0f/eta);

			if (use_fresnel && initial_outside)
				eval2 += throughput2 * phase * mf_G1(wo_outside ? wo : -wo, mf_C1((outside == wo_outside) ? hr : -hr), shadowing_lambda);
#elif defined(MF_MULTI_DIFFUSE)
			phase = mf_eval_phase_diffuse(wo, wm);
#else /* MF_MULTI_GLOSSY */
			phase = mf_eval_phase_glossy(wr, lambda_r, wo, alpha, n, k) * throughput;

			if (use_fresnel)
				eval2 += throughput2 * phase * mf_G1(wo_outside ? wo : -wo, mf_C1((outside == wo_outside) ? hr : -hr), shadowing_lambda);
#endif
			eval += throughput * phase * mf_G1(wo_outside? wo: -wo, mf_C1((outside == wo_outside)? hr: -hr), shadowing_lambda);
		}
		if(order+1 < 10) {
			/* Bounce from the microfacet. */
#ifdef MF_MULTI_GLASS
			bool next_outside;
			float3 wi_prev = -wr;
			wr = mf_sample_phase_glass(-wr, outside? eta: 1.0f/eta, wm, lcg_step_float_addrspace(lcg_state), &next_outside);
			if(!next_outside) {
				outside = !outside;
				wr = -wr;
				hr = -hr;
			}

			if (use_fresnel && initial_outside && outside && next_outside) {
				float FH = (fresnel_dielectric_cos(dot(wi_prev, wm), eta) - F0) * F0_norm; //schlick_fresnel(dot(wi_prev, wm)); //
				t_color = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

				if (order > 0)
					throughput2 *= t_color;
			}
			else {
				throughput2 *= color;
			}
#elif defined(MF_MULTI_DIFFUSE)
			wr = mf_sample_phase_diffuse(wm,
			                             lcg_step_float_addrspace(lcg_state),
			                             lcg_step_float_addrspace(lcg_state));
#else /* MF_MULTI_GLOSSY */
			if (use_fresnel) {
				float FH = (fresnel_dielectric_cos(dot(-wr, wm), eta) - F0) * F0_norm; //schlick_fresnel(dot(-wr, wm)); //
				t_color = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

				if (order > 0)
					throughput2 *= t_color;
			}
			else {
				throughput2 *= color;
			}
			wr = mf_sample_phase_glossy(-wr, n, k, &throughput, wm);
#endif

			lambda_r = mf_lambda(wr, alpha);

			throughput *= color;

			C1_r = mf_C1(hr);
			G1_r = mf_G1(wr, C1_r, lambda_r);
		}
	}

#if defined(MF_MULTI_GLASS) || defined(MF_MULTI_GLOSSY)
	if (use_fresnel) {
		if (swapped)
			eval2 *= fabsf(wi.z / wo.z);

		return eval2;
	}
#endif

	if(swapped)
		eval *= fabsf(wi.z / wo.z);
	return eval;
}

/* Perform a random walk on the microsurface starting from wi, returning the direction in which the walk
 * escaped the surface in wo. The function returns the throughput between wi and wo.
 * Without reflection losses due to coloring or fresnel absorption in conductors, the sampling is optimal.
 */
ccl_device float3 MF_FUNCTION_FULL_NAME(mf_sample)(float3 wi, float3 *wo, const float3 color, const float3 cspec0, const float alpha_x, const float alpha_y, ccl_addr_space uint *lcg_state
#ifdef MF_MULTI_GLASS
	, const float eta
	, bool use_fresnel = false
	, bool initial_outside = true
	, bool only_refractions = false
	, bool only_reflections = false
#elif defined(MF_MULTI_GLOSSY)
	, float3 *n, float3 *k
	, const float eta = 1.0f
	, bool use_fresnel = false
#endif
)
{
	const float2 alpha = make_float2(alpha_x, alpha_y);

	float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
	float3 wr = -wi;
	float lambda_r = mf_lambda(wr, alpha);
	float hr = 1.0f;
	float C1_r = 1.0f;
	float G1_r = 0.0f;
	bool outside = true;
	float3 t_color = cspec0;
	float3 throughput2 = make_float3(1.0f, 1.0f, 1.0f);
	float F0 = fresnel_dielectric_cos(1.0f, eta);
	float F0_norm = 1.0f / (1.0f - F0);
#ifdef MF_MULTI_GLASS
	if (use_fresnel && initial_outside) {
#else
	if (use_fresnel) {
#endif
		float FH = (fresnel_dielectric_cos(dot(wi, normalize(wi + wr)), eta) - F0) * F0_norm; //schlick_fresnel(dot(wi, normalize(wi + wr))); //
		throughput2 = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;
	}

	int order;
	for(order = 0; order < 10; order++) {
		/* Sample microfacet height. */
		if(!mf_sample_height(wr, &hr, &C1_r, &G1_r, &lambda_r, lcg_step_float_addrspace(lcg_state))) {
			/* The random walk has left the surface. */
#ifdef MF_MULTI_GLASS
			if ((only_refractions && outside && initial_outside) || (only_reflections && !outside)) {
				*wo = make_float3(0.0f, 0.0f, 1.0f);
				return make_float3(0.0f, 0.0f, 0.0f);
			}
#endif
			*wo = outside? wr: -wr;
#if defined(MF_MULTI_GLASS) || defined(MF_MULTI_GLOSSY)
			if (use_fresnel)
				return throughput2;
#endif
			return throughput;
		}
		/* Sample microfacet normal. */
		float3 wm = mf_sample_vndf(-wr, alpha, make_float2(lcg_step_float_addrspace(lcg_state),
		                                                   lcg_step_float_addrspace(lcg_state)));

		/* First-bounce color is already accounted for in mix weight. */
		if(order > 0)
			throughput *= color;

		/* Bounce from the microfacet. */
#ifdef MF_MULTI_GLASS
		bool next_outside;
		float3 wi_prev = -wr;
		wr = mf_sample_phase_glass(-wr, outside? eta: 1.0f/eta, wm, lcg_step_float_addrspace(lcg_state), &next_outside);
		if(!next_outside) {
			hr = -hr;
			wr = -wr;
			outside = !outside;
		}

		if (use_fresnel) {
			if (initial_outside && outside && next_outside) {
				float FH = (fresnel_dielectric_cos(dot(wi_prev, wm), eta) - F0) * F0_norm; //schlick_fresnel(dot(wi_prev, wm)); //
				t_color = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

				if (order == 0)
					throughput2 = t_color;
				else
					throughput2 *= t_color;
			}
			else {
				throughput2 *= color;
			}
		}
#elif defined(MF_MULTI_DIFFUSE)
		wr = mf_sample_phase_diffuse(wm,
		                             lcg_step_float_addrspace(lcg_state),
		                             lcg_step_float_addrspace(lcg_state));
#else /* MF_MULTI_GLOSSY */
		if (use_fresnel) {
			float FH = (fresnel_dielectric_cos(dot(-wr, wm), eta) - F0) * F0_norm; //schlick_fresnel(dot(-wr, wm)); //
			t_color = cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH;

			if (order == 0)
				throughput2 = t_color;
			else
				throughput2 *= t_color;
		}
		wr = mf_sample_phase_glossy(-wr, n, k, &throughput, wm);
#endif

		/* Update random walk parameters. */
		lambda_r = mf_lambda(wr, alpha);
		G1_r = mf_G1(wr, C1_r, lambda_r);
	}
	*wo = make_float3(0.0f, 0.0f, 1.0f);
	return make_float3(0.0f, 0.0f, 0.0f);
}

#undef MF_MULTI_GLASS
#undef MF_MULTI_DIFFUSE
#undef MF_MULTI_GLOSSY
#undef MF_PHASE_FUNCTION
