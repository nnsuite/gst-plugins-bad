/* GStreamer
 * Copyright (C) 2007 Sebastien Moutte <sebastien@moutte.net>
 *
 * gstdshow.cpp:
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gstdshow.h"
#include "gstdshowfakesink.h"

void
gst_dshow_free_mediatype (AM_MEDIA_TYPE * pmt)
{
  if (pmt != NULL) {
    if (pmt->cbFormat != 0) {
      CoTaskMemFree ((PVOID) pmt->pbFormat);
      pmt->cbFormat = 0;
      pmt->pbFormat = NULL;
    }
    if (pmt->pUnk != NULL) {
      /* Unecessary because pUnk should not be used, but safest. */
      pmt->pUnk->Release ();
      pmt->pUnk = NULL;
    }

    CoTaskMemFree (pmt);
  }
}

void
gst_dshow_free_pin_mediatype (gpointer pt)
{
  GstCapturePinMediaType *pin_mediatype = (GstCapturePinMediaType *) pt;
  if (pin_mediatype) {
    if (pin_mediatype->capture_pin) {
      pin_mediatype->capture_pin->Release ();
      pin_mediatype->capture_pin = NULL;
    }
    if (pin_mediatype->mediatype) {
      gst_dshow_free_mediatype (pin_mediatype->mediatype);
      pin_mediatype->mediatype = NULL;
    }
  }
}

GstCapturePinMediaType *
gst_dshow_new_pin_mediatype (IPin * pin, gint id, IAMStreamConfig * streamcaps)
{
  GstCapturePinMediaType *pin_mediatype = g_new0 (GstCapturePinMediaType, 1);

  HRESULT hres = streamcaps->GetStreamCaps (id, &pin_mediatype->mediatype,
      (BYTE *) & pin_mediatype->vscc);
  if (FAILED (hres) || !pin_mediatype->mediatype) {
    gst_dshow_free_pin_mediatype (pin_mediatype);
    return NULL;
  }

  pin->AddRef ();
  pin_mediatype->capture_pin = pin;

  return pin_mediatype;
}

void
gst_dshow_free_pins_mediatypes (GList * pins_mediatypes)
{
  guint i = 0;
  for (; i < g_list_length (pins_mediatypes); i++) {
    GList *mylist = g_list_nth (pins_mediatypes, i);
    if (mylist && mylist->data)
      gst_dshow_free_pin_mediatype ((GstCapturePinMediaType *) mylist->data);
  }
  g_list_free (pins_mediatypes);
}

gboolean
gst_dshow_check_mediatype (AM_MEDIA_TYPE * media_type, const GUID sub_type,
    const GUID format_type)
{
  RPC_STATUS rpcstatus;

  g_return_val_if_fail (media_type != NULL, FALSE);

  return
      UuidCompare (&media_type->subtype, (UUID *) & sub_type,
      &rpcstatus) == 0 && rpcstatus == RPC_S_OK &&
      UuidCompare (&media_type->formattype, (UUID *) & format_type,
      &rpcstatus) == 0 && rpcstatus == RPC_S_OK;
}

gboolean
gst_dshow_get_pin_from_filter (IBaseFilter * filter, PIN_DIRECTION pindir,
    IPin ** pin)
{
  gboolean ret = FALSE;
  IEnumPins *enumpins = NULL;
  IPin *pintmp = NULL;
  HRESULT hres;
  *pin = NULL;

  hres = filter->EnumPins (&enumpins);
  if (FAILED (hres)) {
    return ret;
  }

  while (enumpins->Next (1, &pintmp, NULL) == S_OK) {
    PIN_DIRECTION pindirtmp;
    hres = pintmp->QueryDirection (&pindirtmp);
    if (hres == S_OK && pindir == pindirtmp) {
      *pin = pintmp;
      ret = TRUE;
      break;
    }
    pintmp->Release ();
  }
  enumpins->Release ();

  return ret;
}

gboolean
gst_dshow_find_filter (CLSID input_majortype, CLSID input_subtype,
    CLSID output_majortype, CLSID output_subtype,
    gchar * prefered_filter_name, IBaseFilter ** filter)
{
  gboolean ret = FALSE;
  HRESULT hres;
  GUID arrayInTypes[2];
  GUID arrayOutTypes[2];
  IFilterMapper2 *mapper = NULL;
  IEnumMoniker *enum_moniker = NULL;
  IMoniker *moniker = NULL;
  ULONG fetched;
  gchar *prefered_filter_upper = NULL;
  gboolean exit = FALSE;

  /* initialize output parameter */
  if (filter)
    *filter = NULL;

  /* create a private copy of prefered filter substring in upper case */
  if (prefered_filter_name) {
    prefered_filter_upper = g_strdup (prefered_filter_name);
    _strupr (prefered_filter_upper);
  }

  hres = CoCreateInstance (CLSID_FilterMapper2, NULL, CLSCTX_INPROC,
      IID_IFilterMapper2, (void **) &mapper);
  if (FAILED (hres))
    goto clean;

  memcpy (&arrayInTypes[0], &input_majortype, sizeof (CLSID));
  memcpy (&arrayInTypes[1], &input_subtype, sizeof (CLSID));
  memcpy (&arrayOutTypes[0], &output_majortype, sizeof (CLSID));
  memcpy (&arrayOutTypes[1], &output_subtype, sizeof (CLSID));

  hres =
      mapper->EnumMatchingFilters (&enum_moniker, 0, FALSE,
      MERIT_DO_NOT_USE + 1, TRUE, 1, arrayInTypes, NULL, NULL, FALSE, TRUE, 1,
      arrayOutTypes, NULL, NULL);
  if (FAILED (hres))
    goto clean;

  enum_moniker->Reset ();

  while (hres = enum_moniker->Next (1, &moniker, &fetched), hres == S_OK
      && !exit) {
    IBaseFilter *filter_temp = NULL;
    IPropertyBag *property_bag = NULL;
    gchar *friendly_name = NULL;

    hres =
        moniker->BindToStorage (NULL, NULL, IID_IPropertyBag,
        (void **) &property_bag);
    if (SUCCEEDED (hres) && property_bag) {
      VARIANT varFriendlyName;
      VariantInit (&varFriendlyName);

      hres = property_bag->Read (L"FriendlyName", &varFriendlyName, NULL);
      if (hres == S_OK && varFriendlyName.bstrVal) {
        friendly_name =
            g_utf16_to_utf8 ((const gunichar2 *) varFriendlyName.bstrVal,
            wcslen (varFriendlyName.bstrVal), NULL, NULL, NULL);
        if (friendly_name)
          _strupr (friendly_name);
        SysFreeString (varFriendlyName.bstrVal);
      }
      property_bag->Release ();
    }

    hres =
        moniker->BindToObject (NULL, NULL, IID_IBaseFilter,
        (void **) &filter_temp);
    if (SUCCEEDED (hres) && filter_temp) {
      ret = TRUE;
      if (filter) {
        if (*filter)
          (*filter)->Release ();

        *filter = filter_temp;
        (*filter)->AddRef ();

        if (prefered_filter_upper && friendly_name &&
            strstr (friendly_name, prefered_filter_upper))
          exit = TRUE;
      }

      /* if we just want to know if the formats are supported OR
         if we don't care about what will be the filter used
         => we can stop enumeration */
      if (!filter || !prefered_filter_upper)
        exit = TRUE;

      filter_temp->Release ();
    }

    if (friendly_name)
      g_free (friendly_name);
    moniker->Release ();
  }

clean:
  if (prefered_filter_upper)
    g_free (prefered_filter_upper);
  if (enum_moniker)
    enum_moniker->Release ();
  if (mapper)
    mapper->Release ();

  return ret;
}


gchar *
gst_dshow_getdevice_from_devicename (const GUID * device_category,
    gchar ** device_name)
{
  gchar *ret = NULL;
  ICreateDevEnum *devices_enum = NULL;
  IEnumMoniker *enum_moniker = NULL;
  IMoniker *moniker = NULL;
  HRESULT hres = S_FALSE;
  ULONG fetched;
  gboolean bfound = FALSE;

  hres = CoCreateInstance (CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
      IID_ICreateDevEnum, (void **) &devices_enum);
  if (hres != S_OK) {
    /*error */
    goto clean;
  }

  hres = devices_enum->CreateClassEnumerator (*device_category,
      &enum_moniker, 0);
  if (hres != S_OK || !enum_moniker) {
    /*error */
    goto clean;
  }

  enum_moniker->Reset ();

  while (hres = enum_moniker->Next (1, &moniker, &fetched), hres == S_OK
      && !bfound) {
    IPropertyBag *property_bag = NULL;
    hres =
        moniker->BindToStorage (NULL, NULL, IID_IPropertyBag,
        (void **) &property_bag);
    if (SUCCEEDED (hres) && property_bag) {
      VARIANT varFriendlyName;
      VariantInit (&varFriendlyName);

      hres = property_bag->Read (L"FriendlyName", &varFriendlyName, NULL);
      if (hres == S_OK && varFriendlyName.bstrVal) {
        gchar *friendly_name =
            g_utf16_to_utf8 ((const gunichar2 *) varFriendlyName.bstrVal,
            wcslen (varFriendlyName.bstrVal), NULL, NULL, NULL);

        if (!*device_name) {
          *device_name = g_strdup (friendly_name);
        }

        if (_stricmp (*device_name, friendly_name) == 0) {
          WCHAR *wszDisplayName = NULL;
          hres = moniker->GetDisplayName (NULL, NULL, &wszDisplayName);
          if (hres == S_OK && wszDisplayName) {
            ret = g_utf16_to_utf8 ((const gunichar2 *) wszDisplayName,
                wcslen (wszDisplayName), NULL, NULL, NULL);
            CoTaskMemFree (wszDisplayName);
          }
          bfound = TRUE;
        }
        SysFreeString (varFriendlyName.bstrVal);
      }
      property_bag->Release ();
    }
    moniker->Release ();
  }

clean:
  if (enum_moniker) {
    enum_moniker->Release ();
  }

  if (devices_enum) {
    devices_enum->Release ();
  }

  return ret;
}

gboolean
gst_dshow_show_propertypage (IBaseFilter * base_filter)
{
  gboolean ret = FALSE;
  ISpecifyPropertyPages *pProp = NULL;
  HRESULT hres =
      base_filter->QueryInterface (IID_ISpecifyPropertyPages, (void **) &pProp);
  if (SUCCEEDED (hres)) {
    /* Get the filter's name and IUnknown pointer. */
    FILTER_INFO FilterInfo;
    CAUUID caGUID;
    IUnknown *pFilterUnk = NULL;
    hres = base_filter->QueryFilterInfo (&FilterInfo);
    base_filter->QueryInterface (IID_IUnknown, (void **) &pFilterUnk);

    /* Show the page. */
    pProp->GetPages (&caGUID);
    pProp->Release ();
    OleCreatePropertyFrame (GetDesktopWindow (), 0, 0, FilterInfo.achName,
        1, &pFilterUnk, caGUID.cElems, caGUID.pElems, 0, 0, NULL);

    pFilterUnk->Release ();
    FilterInfo.pGraph->Release ();
    CoTaskMemFree (caGUID.pElems);
  }
  return ret;
}

GstCaps *
gst_dshow_new_video_caps (GstVideoFormat video_format, const gchar * name,
    GstCapturePinMediaType * pin_mediatype)
{
  GstCaps *video_caps = NULL;
  GstStructure *video_structure = NULL;
  VIDEOINFOHEADER *video_info =
      (VIDEOINFOHEADER *) pin_mediatype->mediatype->pbFormat;

  pin_mediatype->defaultWidth = video_info->bmiHeader.biWidth;
  pin_mediatype->defaultHeight = video_info->bmiHeader.biHeight;
  pin_mediatype->defaultFPS = (gint) (10000000 / video_info->AvgTimePerFrame);
  pin_mediatype->granularityWidth = pin_mediatype->vscc.OutputGranularityX;
  pin_mediatype->granularityHeight = pin_mediatype->vscc.OutputGranularityY;

  /* raw video format */
  switch (video_format) {
    case GST_VIDEO_FORMAT_BGR:
      video_caps = gst_caps_from_string (GST_VIDEO_CAPS_BGR);
      break;
    case GST_VIDEO_FORMAT_I420:
      video_caps = gst_caps_from_string (GST_VIDEO_CAPS_YUV ("I420"));
    default:
      break;
  }

  /* other video format */
  if (!video_caps) {
    if (g_strcasecmp (name, "video/x-dv, systemstream=FALSE") == 0) {
      video_caps = gst_caps_new_simple ("video/x-dv",
          "systemstream", G_TYPE_BOOLEAN, FALSE,
          "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('d', 'v', 's', 'd'),
          NULL);
    } else if (g_strcasecmp (name, "video/x-dv, systemstream=TRUE") == 0) {
      video_caps = gst_caps_new_simple ("video/x-dv",
          "systemstream", G_TYPE_BOOLEAN, TRUE, NULL);
      return video_caps;
    }
  }

  if (!video_caps)
    return NULL;

  video_structure = gst_caps_get_structure (video_caps, 0);

  /* Hope GST_TYPE_INT_RANGE_STEP will exits in future gstreamer releases  */
  /* because we could use :  */
  /* "width", GST_TYPE_INT_RANGE_STEP, video_default->minWidth, video_default->maxWidth,  video_default->granularityWidth */
  /* instead of : */
  /* "width", GST_TYPE_INT_RANGE, video_default->minWidth, video_default->maxWidth */

  /* For framerate we do not need a step (granularity) because  */
  /* "The IAMStreamConfig::SetFormat method will set the frame rate to the closest  */
  /* value that the filter supports" as it said in the VIDEO_STREAM_CONFIG_CAPS dshwo doc */

  gst_structure_set (video_structure,
      "width", GST_TYPE_INT_RANGE, pin_mediatype->vscc.MinOutputSize.cx,
      pin_mediatype->vscc.MaxOutputSize.cx, "height", GST_TYPE_INT_RANGE,
      pin_mediatype->vscc.MinOutputSize.cy,
      pin_mediatype->vscc.MaxOutputSize.cy, "framerate",
      GST_TYPE_FRACTION_RANGE,
      (gint) (10000000 / pin_mediatype->vscc.MaxFrameInterval), 1,
      (gint) (10000000 / pin_mediatype->vscc.MinFrameInterval), 1, NULL);

  return video_caps;
}
