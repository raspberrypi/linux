// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_kunit_helpers.h>
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
	DRM_FORMAT_XRGB8888,
};

struct drm_plane *vc4_dummy_plane(struct kunit *test, struct drm_device *drm,
				  enum drm_plane_type type)
{
	struct drm_plane *plane;

	KUNIT_ASSERT_EQ(test, type, DRM_PLANE_TYPE_PRIMARY);

	plane = drm_kunit_helper_create_primary_plane(test, drm,
						      NULL,
						      NULL,
						      NULL, 0,
						      NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane);

	return plane;
}
