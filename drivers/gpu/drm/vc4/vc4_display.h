/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

struct intel_encoder {
	struct drm_encoder base;
};

struct vc4_connector {
	struct drm_connector base;

	/* The fixed encoder this connector is connected to. */
	struct drm_encoder *encoder;
};

struct vc4_crtc {
	struct drm_crtc base;
};

#define to_vc4_crtc(x) container_of(x, struct vc4_crtc, base)
#define to_vc4_connector(x) container_of(x, struct vc4_connector, base)
#define to_vc4_encoder(x) container_of(x, struct vc4_encoder, base)

static inline struct drm_encoder *
vc4_attached_encoder(struct drm_connector *connector)
{
	return to_vc4_connector(connector)->encoder;
}
