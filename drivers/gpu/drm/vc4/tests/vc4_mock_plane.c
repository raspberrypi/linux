// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_plane.h>

#include <kunit/test.h>

#include "vc4_mock.h"

static const struct drm_plane_helper_funcs vc4_dummy_plane_helper_funcs = {
	.atomic_check = vc4_plane_atomic_check,
};

static const struct drm_plane_funcs vc4_dummy_plane_funcs = {
	.atomic_destroy_state	= vc4_plane_destroy_state,
	.atomic_duplicate_state	= vc4_plane_duplicate_state,
	.reset			= vc4_plane_reset,
};

static const uint32_t vc4_dummy_plane_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YUV422,
};

struct drm_plane *vc4_dummy_plane(struct kunit *test, struct drm_device *drm,
				  enum drm_plane_type type)
{
	struct drm_plane *plane;

	KUNIT_ASSERT_EQ(test, type, DRM_PLANE_TYPE_PRIMARY);

	plane = drm_kunit_helper_create_primary_plane(test, drm,
						      &vc4_dummy_plane_funcs,
						      &vc4_dummy_plane_helper_funcs,
						      vc4_dummy_plane_formats,
						      ARRAY_SIZE(vc4_dummy_plane_formats),
						      NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane);

	return plane;
}

struct drm_plane *
vc4_mock_atomic_add_plane(struct kunit *test,
			  struct drm_atomic_state *state,
			  struct drm_crtc *crtc)
{
	struct drm_plane_state *plane_state;
	struct drm_plane *plane;
	int ret;

	plane = vc4_mock_find_plane_for_crtc(test, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane);

	plane_state = drm_atomic_get_plane_state(state, plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane_state);

	ret = drm_atomic_set_crtc_for_plane(plane_state, crtc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	return plane;
}
