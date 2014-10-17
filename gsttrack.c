/* GStreamer
 * Copyright (C) 2014 Henry Kroll <nospam@thenerdshow.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gsttrack
 *
 * The track element tracks and optionally marks areas of color in a 
 * video stream. A threshold value may be adjusted to eliminate false 
 * signals. The algorithm gives more weight to color, especialy skin 
 * tones, and pays less attention to shading and bad lighting.
 * During each frame, if the #GstTrack:message property is #TRUE,
 * track emits an element message for each video frame, named
 *
 * <classname>&quot;track&quot;</classname>
 *
 * The message's structure contains these fields:
 * <itemizedlist>
 * <listitem>
 *   <para>
 *   #GstValue of #guint
 *   <classname>&quot;count&quot;</classname>:
 *   Total number of detected objects on screen.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstValue of #guint
 *   <classname>&quot;object&quot;</classname>:
 *   the temporary tracking number of each detected object.
 *   Note: Tracking numbers are initially assigned in top-down and
 *    left-to-right order. Track will attempt to assign the same
 *    tracking numbers to the same objects on each video frame,
 *    except if an object loses focus or leaves the frame.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstValueList of #guint
 *   <classname>&quot;x1,y1,x2,y2&quot;</classname>:
 *   the x,y coordinates of the top-left and bottom-right corner
 *   of the bounding box for each detected object.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstValueList of #guint
 *   <classname>&quot;xc,yc&quot;</classname>:
 *   the x,y coordinates of the center of each detected object.
 *   </para>
 * </listitem>
 * </itemizedlist>
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v v4l2src ! track ! autovideoconvert ! autovideosink
 * ]|
 * Insert track between video src and sink elements and set color 
 * properties. The trackable objects' primary, or background color 
 * is most important. This is the first color that track finds. 
 * The foreground, text, and outline colors are optional. Track's 
 * algorithm can now track team players. Track can follow
 * blue players with black and orange markings, while ignoring orange 
 * players with black and blue markings on the field. Set the 
 * object's (player's) background color to blue and the foreground 
 * colors to black, and orange. Want to track the other team, too? 
 * No problem! Multiple instances of track may be added to the 
 * pipeline.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <math.h>
#include "gsttrack.h"
#include "hkgraphics.h"

GST_DEBUG_CATEGORY_STATIC (gst_track_debug_category);
#define GST_CAT_DEFAULT gst_track_debug_category

/* prototypes */

static void gst_track_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_track_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_track_finalize (GObject * object);

static gboolean gst_track_start (GstBaseTransform * trans);
static gboolean gst_track_stop (GstBaseTransform * trans);

static GstFlowReturn
gst_track_prefilter (GstVideoFilter2 * videofilter2, GstBuffer * buf);

static GstVideoFilter2Functions gst_track_filter_functions[];

/* class initialization */

#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (gst_track_debug_category, "track", 0, \
      "debug category for track element");

#define GST_TYPE_TRACK_REPLACE_METHOD (gst_track_replace_method_get_type())

static const GEnumValue repace_methods[] = {
  {GST_TRACK_REPLACE_METHOD_NOTHING, "Do nothing", "nothing"},
  {GST_TRACK_REPLACE_METHOD_CROSSHAIRS, "Mark with crosshairs", "crosshairs"},
  {GST_TRACK_REPLACE_METHOD_BOX, "Draw box", "box"},
  {GST_TRACK_REPLACE_METHOD_BOTH, "Both 1 and 2", "both"},
  {GST_TRACK_REPLACE_METHOD_CLOAK, "Cloaking device", "cloak"},
  {GST_TRACK_REPLACE_METHOD_BLUR8, "Blur, 8 x 8 average", "blur"},
  {GST_TRACK_REPLACE_METHOD_BLUR, "Blur, size x size", "sizeblur"},
  {GST_TRACK_REPLACE_METHOD_DECIMATE, "Decimate into squares",
      "decimate"},
  {GST_TRACK_REPLACE_METHOD_TOONIFY, "Cartoon to bgcolor",
      "toonify"},
  {0, NULL, NULL},
};

static GType
gst_track_replace_method_get_type (void)
{
  static GType repace_method_type = 0;
  if (!repace_method_type) {
    repace_method_type = g_enum_register_static ("GstTrackReplaceMethod",
        repace_methods);
  }
  return repace_method_type;
}

GST_BOILERPLATE_FULL (GstTrack, gst_track, GstVideoFilter2,
    GST_TYPE_VIDEO_FILTER2, DEBUG_INIT);

static void
gst_track_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  gst_element_class_set_details_simple (element_class, "Object tracking / marking",
      "Filter/Tracking",
      "The track element tracks and optionally marks areas of color in a video stream.",
    "Henry Kroll III, www.thenerdshow.com");
}

static void
gst_track_class_init (GstTrackClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstVideoFilter2Class *video_filter2_class = GST_VIDEO_FILTER2_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_track_set_property;
  gobject_class->get_property = gst_track_get_property;
  gobject_class->finalize = gst_track_finalize;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_track_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_track_stop);

  video_filter2_class->prefilter =
      GST_DEBUG_FUNCPTR (gst_track_prefilter);

  g_object_class_install_property (gobject_class, PROP_MESSAGE,
      g_param_spec_boolean ("message", "message",
          "Post a message for each tracked object",
        DEFAULT_MESSAGE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_REPLACE_METHOD,
      g_param_spec_enum ("replace", "Replace Method", "What to replace tracked objects with",
          GST_TYPE_TRACK_REPLACE_METHOD, DEFAULT_REPLACE_METHOD,
          GST_PARAM_CONTROLLABLE | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_THRESHOLD,
      g_param_spec_uint ("threshold", "Threshold",
          "Tracking color difference threshold", 0, 600,
          DEFAULT_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_OBJECTS,
      g_param_spec_uint ("objects", "Objects",
          "Max number of objects to track", 1, MAX_OBJECTS,
          DEFAULT_MAX_OBJECTS,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SIZE,
      g_param_spec_uint ("size", "Size",
          "Minimum size of objects to track", 1, 100,
          DEFAULT_SIZE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BGCOLOR,
      g_param_spec_uint ("bgcolor", "Background Color",
          "Object's Main or Background Color red=0xff0000", 0, G_MAXUINT,
          DEFAULT_COLOR,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FGCOLOR0,
      g_param_spec_uint ("fgcolor0", "Foreground Color 0",
          "Object's Highlight or Text Color green=0x00ff00", 0, G_MAXUINT,
          DEFAULT_COLOR,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FGCOLOR0,
      g_param_spec_uint ("fgcolor1", "Foreground Color 1",
          "Object's Spot or Outline Color blue=0x0000ff", 0, G_MAXUINT,
          DEFAULT_COLOR,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  gst_video_filter2_class_add_functions (video_filter2_class,
      gst_track_filter_functions);
}

static void
gst_track_init (GstTrack * track,
    GstTrackClass * track_class)
{
  track->message = DEFAULT_MESSAGE;
  track->bgcolor = DEFAULT_COLOR;
  track->fgcolor0 = DEFAULT_COLOR;
  track->fgcolor1 = DEFAULT_COLOR;
  track->threshold = DEFAULT_THRESHOLD;
  track->max_objects = DEFAULT_MAX_OBJECTS;
  track->replace_method = DEFAULT_REPLACE_METHOD;
  track->obj_count = 0;
  for (int obj=MAX_OBJECTS; obj--;)
    track->obj_found[obj][3] = 0;
}

void
gst_track_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTrack *track;
  g_return_if_fail (GST_IS_TRACK (object));
  track = GST_TRACK (object);

  switch (property_id) {
    case PROP_MESSAGE:
      track->message = g_value_get_boolean(value);
      break;
    case PROP_SIZE:
      track->size = g_value_get_uint(value);
      break;
    case PROP_BGCOLOR:
      track->bgcolor = g_value_get_uint(value);
      rgb2yuv(track->bgcolor, track->bgyuv);
      break;
    case PROP_FGCOLOR0:
      track->fgcolor0 = g_value_get_uint(value);
      rgb2yuv(track->fgcolor0, track->fgyuv0);
      break;
    case PROP_FGCOLOR1:
      track->fgcolor1 = g_value_get_uint(value);
      rgb2yuv(track->fgcolor1, track->fgyuv1);
      break;
    case PROP_THRESHOLD:
      track->threshold = g_value_get_uint(value);
      break;
    case PROP_MAX_OBJECTS:
      track->max_objects = g_value_get_uint(value);
      break;
    case PROP_REPLACE_METHOD:
      track->replace_method = g_value_get_enum(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_track_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstTrack *track;

  g_return_if_fail (GST_IS_TRACK (object));
  track = GST_TRACK (object);

  switch (property_id) {
    case PROP_MESSAGE:
      g_value_set_boolean (value, track->message);
      break;
    case PROP_SIZE:
      g_value_set_uint (value, track->size);
      break;
    case PROP_BGCOLOR:
      g_value_set_uint (value, track->bgcolor);
      break;
    case PROP_FGCOLOR0:
      g_value_set_uint (value, track->fgcolor0);
      break;
    case PROP_FGCOLOR1:
      g_value_set_uint (value, track->fgcolor1);
      break;
    case PROP_THRESHOLD:
      g_value_set_uint (value, track->threshold);
      break;
    case PROP_MAX_OBJECTS:
      g_value_set_uint (value, track->max_objects);
      break;
    case PROP_REPLACE_METHOD:
      g_value_set_enum (value, track->replace_method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_track_finalize (GObject * object)
{
  g_return_if_fail (GST_IS_TRACK (object));

  /* clean up object here */

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static gboolean
gst_track_start (GstBaseTransform * trans)
{
  return TRUE;
}

static gboolean
gst_track_stop (GstBaseTransform * trans)
{
  return TRUE;
}

static GstFlowReturn
gst_track_prefilter (GstVideoFilter2 * videofilter2, GstBuffer * buf)
{
  return GST_FLOW_OK;
}

static void hkgraphics_init (GstTrack *track, hkVidLayout *vl, GstBuffer *buf)
/* populate hkVidLayout struct for hkgraphics library */
/* called upon each video frame */
{
  GstVideoFormat format = GST_VIDEO_FILTER2_FORMAT (track);
  guint8 *gdata = GST_BUFFER_DATA (buf);
  vl->width = GST_VIDEO_FILTER2_WIDTH (track),
  vl->height = GST_VIDEO_FILTER2_HEIGHT (track),
  vl->threshold = track->threshold,
  vl->bgcolor = track->bgyuv,
  vl->fgcolor0 = track->fgyuv0;
  vl->fgcolor1 = track->fgyuv1;
  for (int i=3;i--;){
    vl->data[i] = gdata + gst_video_format_get_component_offset
      (format, i, vl->width, vl->height);
    vl->stride[i] = gst_video_format_get_row_stride
      (format, i, vl->width);
    vl->hscale[i] = vl->height /
      gst_video_format_get_component_height (format, i, vl->height);
    vl->wscale[i] = vl->width /
      gst_video_format_get_component_width (format, i, vl->width);
  }
}

static gboolean is_reject(GstTrack *track, guint *rect, guint obj)
{
  gboolean reject = FALSE;
  // too small?
  if (rect[2]-rect[0] < track->size
    || rect[3]-rect[1] < track->size){
    reject = TRUE;
  }
  // already detected?
  for (int o=MAX_OBJECTS; o--;){
    if (o==obj || !track->obj_found[o][3]) continue;
    if (track->obj_found[o][0]==rect[0]
      && track->obj_found[o][1]==rect[1]){ 
        reject = TRUE;
        break;
    }
  }
  return reject;
}

static void scan_for_objects(GstTrack *track, hkVidLayout *vl)
/* search video frame and count any colored objects */
{
  guint size = track->size,
    max = track->max_objects,
    rect[4]={0}, *center,
    available = 0;
  for (int i=0; i<vl->height && track->obj_count < max; i+=size){
    for (int j=0;j<vl->width && track->obj_count < max; j+=size){
      if (matchColor(vl, j, i, track->bgyuv)){
        // measure bounds of detected object
        getBounds(vl, j, i, rect);
        if (is_reject(track, rect, MAX_OBJECTS)){
          rect[3] = 0; continue;
        }
        center = rectCenter(rect);
        // find an available obj_found storage location
        for (int i=0; i<max; i++){
          if (!track->obj_found[i][3]){
            available = i;
            break;
          }
        }
        for (int r=4; r--;){
          track->obj_found[available][r] = rect[r];
          rect[r] = 0;
        }
        track->obj_found[available][4] = center[0];
        track->obj_found[available][5] = center[1];
        track->obj_count++;
      }
    }
  }
}

static void track_objects(GstTrack *track, hkVidLayout *vl)
/* Follows existing objects as they move about. */
/* Attempts to keep persistent tracking numbers assigned. */
{
  guint *rect, *center;
  for (int obj = 0; obj < MAX_OBJECTS; obj++){
    rect = track->obj_found[obj];
    if (!rect[3]) continue; // next
    // clear old rect
    for (int i=4; i--;) rect[i] = 0;
    // get new bounds
    getBounds(vl, rect[4], rect[5], rect);
    // check validity
    if (is_reject(track, rect, obj)){
      // reject; wipe it
      track->obj_count--;
      rect[3] = 0; continue; // next
    }
    // get new center
    center = rectCenter(rect);
    rect[4] = center[0];
    rect[5] = center[1];
  }
  scan_for_objects(track, vl);
}

static void report_objects(GstTrack *track, hkVidLayout *vl)
/* report object count, locations, optionally replace */
{
  GstStructure *s;
  guint8 mcolor[3];
  guint *prect, *center, obj = 0;
  // mix up some white paint, for optional markers
  rgb2yuv(0xffffff, mcolor);
  for (int c=track->obj_count; c--;){
    do {
      if (track->obj_found[obj][3]) break;
      obj++;
    } while (1);
    prect = track->obj_found[obj], center = &track->obj_found[obj][4];
    switch (track->replace_method){
      case GST_TRACK_REPLACE_METHOD_BOX:
        box(vl, prect, mcolor);
        break;
      case GST_TRACK_REPLACE_METHOD_BOTH:
        box(vl, prect, mcolor);
      case GST_TRACK_REPLACE_METHOD_CROSSHAIRS:
        crosshairs(vl, center, mcolor);
        break;
      case GST_TRACK_REPLACE_METHOD_CLOAK:
        cloak(vl, prect);
        break;
      case GST_TRACK_REPLACE_METHOD_BLUR:
        blur(vl, prect, track->size);
        break;
      case GST_TRACK_REPLACE_METHOD_BLUR8:
        blur(vl, prect, 8);
        break;
      case GST_TRACK_REPLACE_METHOD_DECIMATE:
        decimate(vl, prect, track->size);
        break;
      case GST_TRACK_REPLACE_METHOD_TOONIFY:
        toonify(vl, prect);
        break;
      default:
        break;
    }
    if (track->message){
      //~ numstr = g_strdup_printf("track[%i]",track->obj_count);
      s = gst_structure_new ("track",
      "count", G_TYPE_UINT, track->obj_count,
      "object", G_TYPE_UINT, obj,
      "x1", G_TYPE_UINT, prect[0],
      "y1", G_TYPE_UINT, prect[1],
      "x2", G_TYPE_UINT, prect[2],
      "y2", G_TYPE_UINT, prect[3],
      "xc", G_TYPE_UINT, center[0],
      "yc", G_TYPE_UINT, center[1],
        NULL);
      gst_element_post_message (GST_ELEMENT_CAST (track),
        gst_message_new_element (GST_OBJECT_CAST (track), s));
      //~ g_free(numstr);
    }
    obj++;
  }
}

static GstFlowReturn
gst_track_filter_ip_planarY (GstVideoFilter2 * videofilter2,
    GstBuffer * buf, int start, int end)
{
  GstTrack *track = GST_TRACK (videofilter2);
  hkVidLayout vl; hkgraphics_init(track, &vl, buf);
  track_objects(track, &vl);
  report_objects(track, &vl);
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_track_filter_ip_YxYy (GstVideoFilter2 * videofilter2,
    GstBuffer * buf, int start, int end)
{
  g_print("Fixme: YxYy\n");
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_track_filter_ip_AYUV (GstVideoFilter2 * videofilter2,
    GstBuffer * buf, int start, int end)
{
  g_print("Fixme: AYUV\n");
  return GST_FLOW_OK;
}

static GstVideoFilter2Functions gst_track_filter_functions[] = {
  {GST_VIDEO_FORMAT_I420, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_YV12, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_Y41B, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_Y42B, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_NV12, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_NV21, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_YUV9, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_YVU9, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_Y444, NULL, gst_track_filter_ip_planarY},
  {GST_VIDEO_FORMAT_UYVY, NULL, gst_track_filter_ip_YxYy},
  {GST_VIDEO_FORMAT_YUY2, NULL, gst_track_filter_ip_YxYy},
  {GST_VIDEO_FORMAT_YVYU, NULL, gst_track_filter_ip_YxYy},
  {GST_VIDEO_FORMAT_AYUV, NULL, gst_track_filter_ip_AYUV},
  {GST_VIDEO_FORMAT_UNKNOWN}
};
