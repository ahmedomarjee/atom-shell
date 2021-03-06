// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/printing/print_web_view_helper.h"

#include <string>

#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/print_messages.h"
#include "content/public/common/web_preferences.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "net/base/escape.h"
#include "printing/pdf_metafile_skia.h"
#include "printing/units.h"
#include "third_party/WebKit/public/platform/WebSize.h"
#include "third_party/WebKit/public/platform/WebURLRequest.h"
#include "third_party/WebKit/public/web/WebConsoleMessage.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebElement.h"
#include "third_party/WebKit/public/web/WebFrameClient.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebPlugin.h"
#include "third_party/WebKit/public/web/WebPluginDocument.h"
#include "third_party/WebKit/public/web/WebPrintParams.h"
#include "third_party/WebKit/public/web/WebPrintScalingOption.h"
#include "third_party/WebKit/public/web/WebScriptSource.h"
#include "third_party/WebKit/public/web/WebSettings.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "third_party/WebKit/public/web/WebViewClient.h"
#include "ui/base/resource/resource_bundle.h"

using content::WebPreferences;

namespace printing {

namespace {

const double kMinDpi = 1.0;

int GetDPI(const PrintMsg_Print_Params* print_params) {
#if defined(OS_MACOSX)
  // On the Mac, the printable area is in points, don't do any scaling based
  // on dpi.
  return kPointsPerInch;
#else
  return static_cast<int>(print_params->dpi);
#endif  // defined(OS_MACOSX)
}

bool PrintMsg_Print_Params_IsValid(const PrintMsg_Print_Params& params) {
  return !params.content_size.IsEmpty() && !params.page_size.IsEmpty() &&
         !params.printable_area.IsEmpty() && params.document_cookie &&
         params.desired_dpi && params.max_shrink && params.min_shrink &&
         params.dpi && (params.margin_top >= 0) && (params.margin_left >= 0) &&
         params.dpi > kMinDpi && params.document_cookie != 0;
}

PrintMsg_Print_Params GetCssPrintParams(
    blink::WebFrame* frame,
    int page_index,
    const PrintMsg_Print_Params& page_params) {
  PrintMsg_Print_Params page_css_params = page_params;
  int dpi = GetDPI(&page_params);

  blink::WebSize page_size_in_pixels(
      ConvertUnit(page_params.page_size.width(), dpi, kPixelsPerInch),
      ConvertUnit(page_params.page_size.height(), dpi, kPixelsPerInch));
  int margin_top_in_pixels =
      ConvertUnit(page_params.margin_top, dpi, kPixelsPerInch);
  int margin_right_in_pixels = ConvertUnit(
      page_params.page_size.width() -
      page_params.content_size.width() - page_params.margin_left,
      dpi, kPixelsPerInch);
  int margin_bottom_in_pixels = ConvertUnit(
      page_params.page_size.height() -
      page_params.content_size.height() - page_params.margin_top,
      dpi, kPixelsPerInch);
  int margin_left_in_pixels = ConvertUnit(
      page_params.margin_left,
      dpi, kPixelsPerInch);

  blink::WebSize original_page_size_in_pixels = page_size_in_pixels;

  if (frame) {
    frame->pageSizeAndMarginsInPixels(page_index,
                                      page_size_in_pixels,
                                      margin_top_in_pixels,
                                      margin_right_in_pixels,
                                      margin_bottom_in_pixels,
                                      margin_left_in_pixels);
  }

  int new_content_width = page_size_in_pixels.width -
                          margin_left_in_pixels - margin_right_in_pixels;
  int new_content_height = page_size_in_pixels.height -
                           margin_top_in_pixels - margin_bottom_in_pixels;

  // Invalid page size and/or margins. We just use the default setting.
  if (new_content_width < 1 || new_content_height < 1) {
    CHECK(frame != NULL);
    page_css_params = GetCssPrintParams(NULL, page_index, page_params);
    return page_css_params;
  }

  page_css_params.content_size = gfx::Size(
      ConvertUnit(new_content_width, kPixelsPerInch, dpi),
      ConvertUnit(new_content_height, kPixelsPerInch, dpi));

  if (original_page_size_in_pixels != page_size_in_pixels) {
    page_css_params.page_size = gfx::Size(
        ConvertUnit(page_size_in_pixels.width, kPixelsPerInch, dpi),
        ConvertUnit(page_size_in_pixels.height, kPixelsPerInch, dpi));
  } else {
    // Printing frame doesn't have any page size css. Pixels to dpi conversion
    // causes rounding off errors. Therefore use the default page size values
    // directly.
    page_css_params.page_size = page_params.page_size;
  }

  page_css_params.margin_top =
      ConvertUnit(margin_top_in_pixels, kPixelsPerInch, dpi);
  page_css_params.margin_left =
      ConvertUnit(margin_left_in_pixels, kPixelsPerInch, dpi);
  return page_css_params;
}

double FitPrintParamsToPage(const PrintMsg_Print_Params& page_params,
                            PrintMsg_Print_Params* params_to_fit) {
  double content_width =
      static_cast<double>(params_to_fit->content_size.width());
  double content_height =
      static_cast<double>(params_to_fit->content_size.height());
  int default_page_size_height = page_params.page_size.height();
  int default_page_size_width = page_params.page_size.width();
  int css_page_size_height = params_to_fit->page_size.height();
  int css_page_size_width = params_to_fit->page_size.width();

  double scale_factor = 1.0f;
  if (page_params.page_size == params_to_fit->page_size)
    return scale_factor;

  if (default_page_size_width < css_page_size_width ||
      default_page_size_height < css_page_size_height) {
    double ratio_width =
        static_cast<double>(default_page_size_width) / css_page_size_width;
    double ratio_height =
        static_cast<double>(default_page_size_height) / css_page_size_height;
    scale_factor = ratio_width < ratio_height ? ratio_width : ratio_height;
    content_width *= scale_factor;
    content_height *= scale_factor;
  }
  params_to_fit->margin_top = static_cast<int>(
      (default_page_size_height - css_page_size_height * scale_factor) / 2 +
      (params_to_fit->margin_top * scale_factor));
  params_to_fit->margin_left = static_cast<int>(
      (default_page_size_width - css_page_size_width * scale_factor) / 2 +
      (params_to_fit->margin_left * scale_factor));
  params_to_fit->content_size = gfx::Size(
      static_cast<int>(content_width), static_cast<int>(content_height));
  params_to_fit->page_size = page_params.page_size;
  return scale_factor;
}

void CalculatePageLayoutFromPrintParams(
    const PrintMsg_Print_Params& params,
    PageSizeMargins* page_layout_in_points) {
  int dpi = GetDPI(&params);
  int content_width = params.content_size.width();
  int content_height = params.content_size.height();

  int margin_bottom = params.page_size.height() -
                      content_height - params.margin_top;
  int margin_right = params.page_size.width() -
                      content_width - params.margin_left;

  page_layout_in_points->content_width =
      ConvertUnit(content_width, dpi, kPointsPerInch);
  page_layout_in_points->content_height =
      ConvertUnit(content_height, dpi, kPointsPerInch);
  page_layout_in_points->margin_top =
      ConvertUnit(params.margin_top, dpi, kPointsPerInch);
  page_layout_in_points->margin_right =
      ConvertUnit(margin_right, dpi, kPointsPerInch);
  page_layout_in_points->margin_bottom =
      ConvertUnit(margin_bottom, dpi, kPointsPerInch);
  page_layout_in_points->margin_left =
      ConvertUnit(params.margin_left, dpi, kPointsPerInch);
}

void EnsureOrientationMatches(const PrintMsg_Print_Params& css_params,
                              PrintMsg_Print_Params* page_params) {
  if ((page_params->page_size.width() > page_params->page_size.height()) ==
      (css_params.page_size.width() > css_params.page_size.height())) {
    return;
  }

  // Swap the |width| and |height| values.
  page_params->page_size.SetSize(page_params->page_size.height(),
                                 page_params->page_size.width());
  page_params->content_size.SetSize(page_params->content_size.height(),
                                    page_params->content_size.width());
  page_params->printable_area.set_size(
      gfx::Size(page_params->printable_area.height(),
                page_params->printable_area.width()));
}

void ComputeWebKitPrintParamsInDesiredDpi(
    const PrintMsg_Print_Params& print_params,
    blink::WebPrintParams* webkit_print_params) {
  int dpi = GetDPI(&print_params);
  webkit_print_params->printerDPI = dpi;
  webkit_print_params->printScalingOption = print_params.print_scaling_option;

  webkit_print_params->printContentArea.width =
      ConvertUnit(print_params.content_size.width(), dpi,
                  print_params.desired_dpi);
  webkit_print_params->printContentArea.height =
      ConvertUnit(print_params.content_size.height(), dpi,
                  print_params.desired_dpi);

  webkit_print_params->printableArea.x =
      ConvertUnit(print_params.printable_area.x(), dpi,
                  print_params.desired_dpi);
  webkit_print_params->printableArea.y =
      ConvertUnit(print_params.printable_area.y(), dpi,
                  print_params.desired_dpi);
  webkit_print_params->printableArea.width =
      ConvertUnit(print_params.printable_area.width(), dpi,
                  print_params.desired_dpi);
  webkit_print_params->printableArea.height =
      ConvertUnit(print_params.printable_area.height(),
                  dpi, print_params.desired_dpi);

  webkit_print_params->paperSize.width =
      ConvertUnit(print_params.page_size.width(), dpi,
                  print_params.desired_dpi);
  webkit_print_params->paperSize.height =
      ConvertUnit(print_params.page_size.height(), dpi,
                  print_params.desired_dpi);
}

blink::WebPlugin* GetPlugin(const blink::WebFrame* frame) {
  return frame->document().isPluginDocument() ?
         frame->document().to<blink::WebPluginDocument>().plugin() : NULL;
}

bool PrintingNodeOrPdfFrame(const blink::WebFrame* frame,
                            const blink::WebNode& node) {
  if (!node.isNull())
    return true;
  blink::WebPlugin* plugin = GetPlugin(frame);
  return plugin && plugin->supportsPaginatedPrint();
}

MarginType GetMarginsForPdf(blink::WebFrame* frame,
                            const blink::WebNode& node) {
  if (frame->isPrintScalingDisabledForPlugin(node))
    return NO_MARGINS;
  else
    return PRINTABLE_AREA_MARGINS;
}

PrintMsg_Print_Params CalculatePrintParamsForCss(
    blink::WebFrame* frame,
    int page_index,
    const PrintMsg_Print_Params& page_params,
    bool ignore_css_margins,
    bool fit_to_page,
    double* scale_factor) {
  PrintMsg_Print_Params css_params = GetCssPrintParams(frame, page_index,
                                                       page_params);

  PrintMsg_Print_Params params = page_params;
  EnsureOrientationMatches(css_params, &params);

  if (ignore_css_margins && fit_to_page)
    return params;

  PrintMsg_Print_Params result_params = css_params;
  if (ignore_css_margins) {
    result_params.margin_top = params.margin_top;
    result_params.margin_left = params.margin_left;

    DCHECK(!fit_to_page);
    // Since we are ignoring the margins, the css page size is no longer
    // valid.
    int default_margin_right = params.page_size.width() -
        params.content_size.width() - params.margin_left;
    int default_margin_bottom = params.page_size.height() -
        params.content_size.height() - params.margin_top;
    result_params.content_size = gfx::Size(
        result_params.page_size.width() - result_params.margin_left -
            default_margin_right,
        result_params.page_size.height() - result_params.margin_top -
            default_margin_bottom);
  }

  if (fit_to_page) {
    double factor = FitPrintParamsToPage(params, &result_params);
    if (scale_factor)
      *scale_factor = factor;
  }
  return result_params;
}

}  // namespace

FrameReference::FrameReference(blink::WebLocalFrame* frame) {
  Reset(frame);
}

FrameReference::FrameReference() {
  Reset(NULL);
}

FrameReference::~FrameReference() {
}

void FrameReference::Reset(blink::WebLocalFrame* frame) {
  if (frame) {
    view_ = frame->view();
    frame_ = frame;
  } else {
    view_ = NULL;
    frame_ = NULL;
  }
}

blink::WebLocalFrame* FrameReference::GetFrame() {
  if (view_ == NULL || frame_ == NULL)
    return NULL;
  for (blink::WebFrame* frame = view_->mainFrame(); frame != NULL;
           frame = frame->traverseNext(false)) {
    if (frame == frame_)
      return frame_;
  }
  return NULL;
}

blink::WebView* FrameReference::view() {
  return view_;
}

// static - Not anonymous so that platform implementations can use it.
float PrintWebViewHelper::RenderPageContent(blink::WebFrame* frame,
                                            int page_number,
                                            const gfx::Rect& canvas_area,
                                            const gfx::Rect& content_area,
                                            double scale_factor,
                                            blink::WebCanvas* canvas) {
  SkAutoCanvasRestore auto_restore(canvas, true);
  if (content_area != canvas_area) {
    canvas->translate((content_area.x() - canvas_area.x()) / scale_factor,
                      (content_area.y() - canvas_area.y()) / scale_factor);
    SkRect clip_rect(
        SkRect::MakeXYWH(content_area.origin().x() / scale_factor,
                         content_area.origin().y() / scale_factor,
                         content_area.size().width() / scale_factor,
                         content_area.size().height() / scale_factor));
    SkIRect clip_int_rect;
    clip_rect.roundOut(&clip_int_rect);
    SkRegion clip_region(clip_int_rect);
    canvas->setClipRegion(clip_region);
  }
  return frame->printPage(page_number, canvas);
}

// Class that calls the Begin and End print functions on the frame and changes
// the size of the view temporarily to support full page printing..
class PrepareFrameAndViewForPrint : public blink::WebViewClient,
                                    public blink::WebFrameClient {
 public:
  PrepareFrameAndViewForPrint(const PrintMsg_Print_Params& params,
                              blink::WebLocalFrame* frame,
                              const blink::WebNode& node,
                              bool ignore_css_margins);
  virtual ~PrepareFrameAndViewForPrint();

  // Optional. Replaces |frame_| with selection if needed. Will call |on_ready|
  // when completed.
  void CopySelectionIfNeeded(const WebPreferences& preferences,
                             const base::Closure& on_ready);

  // Prepares frame for printing.
  void StartPrinting();

  blink::WebLocalFrame* frame() {
    return frame_.GetFrame();
  }

  const blink::WebNode& node() const {
    return node_to_print_;
  }

  int GetExpectedPageCount() const {
    return expected_pages_count_;
  }

  void FinishPrinting();

  bool IsLoadingSelection() {
    // It's not selection if not |owns_web_view_|.
    return owns_web_view_ && frame() && frame()->isLoading();
  }

  // TODO(ojan): Remove this override and have this class use a non-null
  // layerTreeView.
  // blink::WebViewClient override:
  virtual bool allowsBrokenNullLayerTreeView() const;

 protected:
  // blink::WebViewClient override:
  virtual void didStopLoading();

  // blink::WebFrameClient override:
  virtual blink::WebFrame* createChildFrame(blink::WebLocalFrame* parent,
                                            const blink::WebString& name);
  virtual void frameDetached(blink::WebFrame* frame);

 private:
  void CallOnReady();
  void ResizeForPrinting();
  void RestoreSize();
  void CopySelection(const WebPreferences& preferences);

  base::WeakPtrFactory<PrepareFrameAndViewForPrint> weak_ptr_factory_;

  FrameReference frame_;
  blink::WebNode node_to_print_;
  bool owns_web_view_;
  blink::WebPrintParams web_print_params_;
  gfx::Size prev_view_size_;
  gfx::Size prev_scroll_offset_;
  int expected_pages_count_;
  base::Closure on_ready_;
  bool should_print_backgrounds_;
  bool should_print_selection_only_;
  bool is_printing_started_;

  DISALLOW_COPY_AND_ASSIGN(PrepareFrameAndViewForPrint);
};

PrepareFrameAndViewForPrint::PrepareFrameAndViewForPrint(
    const PrintMsg_Print_Params& params,
    blink::WebLocalFrame* frame,
    const blink::WebNode& node,
    bool ignore_css_margins)
    : weak_ptr_factory_(this),
      frame_(frame),
      node_to_print_(node),
      owns_web_view_(false),
      expected_pages_count_(0),
      should_print_backgrounds_(params.should_print_backgrounds),
      should_print_selection_only_(params.selection_only),
      is_printing_started_(false) {
  PrintMsg_Print_Params print_params = params;
  if (!should_print_selection_only_ ||
      !PrintingNodeOrPdfFrame(frame, node_to_print_)) {
    bool fit_to_page = ignore_css_margins &&
                       print_params.print_scaling_option ==
                            blink::WebPrintScalingOptionFitToPrintableArea;
    ComputeWebKitPrintParamsInDesiredDpi(params, &web_print_params_);
    frame->printBegin(web_print_params_, node_to_print_);
    print_params = CalculatePrintParamsForCss(frame, 0, print_params,
                                              ignore_css_margins, fit_to_page,
                                              NULL);
    frame->printEnd();
  }
  ComputeWebKitPrintParamsInDesiredDpi(print_params, &web_print_params_);
}

PrepareFrameAndViewForPrint::~PrepareFrameAndViewForPrint() {
  FinishPrinting();
}

void PrepareFrameAndViewForPrint::ResizeForPrinting() {
  // Layout page according to printer page size. Since WebKit shrinks the
  // size of the page automatically (from 125% to 200%) we trick it to
  // think the page is 125% larger so the size of the page is correct for
  // minimum (default) scaling.
  // This is important for sites that try to fill the page.
  gfx::Size print_layout_size(web_print_params_.printContentArea.width,
                              web_print_params_.printContentArea.height);
  print_layout_size.set_height(
      static_cast<int>(static_cast<double>(print_layout_size.height()) * 1.25));

  if (!frame())
    return;
  blink::WebView* web_view = frame_.view();
  // Backup size and offset.
  if (blink::WebFrame* web_frame = web_view->mainFrame())
    prev_scroll_offset_ = web_frame->scrollOffset();
  prev_view_size_ = web_view->size();

  web_view->resize(print_layout_size);
}


void PrepareFrameAndViewForPrint::StartPrinting() {
  ResizeForPrinting();
  blink::WebView* web_view = frame_.view();
  web_view->settings()->setShouldPrintBackgrounds(should_print_backgrounds_);
  expected_pages_count_ =
      frame()->printBegin(web_print_params_, node_to_print_);
  is_printing_started_ = true;
}

void PrepareFrameAndViewForPrint::CopySelectionIfNeeded(
    const WebPreferences& preferences,
    const base::Closure& on_ready) {
  on_ready_ = on_ready;
  if (should_print_selection_only_) {
    CopySelection(preferences);
  } else {
    // Call immediately, async call crashes scripting printing.
    CallOnReady();
  }
}

void PrepareFrameAndViewForPrint::CopySelection(
    const WebPreferences& preferences) {
  ResizeForPrinting();
  std::string url_str = "data:text/html;charset=utf-8,";
  url_str.append(
      net::EscapeQueryParamValue(frame()->selectionAsMarkup().utf8(), false));
  RestoreSize();
  // Create a new WebView with the same settings as the current display one.
  // Except that we disable javascript (don't want any active content running
  // on the page).
  WebPreferences prefs = preferences;
  prefs.javascript_enabled = false;
  prefs.java_enabled = false;

  blink::WebView* web_view = blink::WebView::create(this);
  owns_web_view_ = true;
  content::RenderView::ApplyWebPreferences(prefs, web_view);
  web_view->setMainFrame(blink::WebLocalFrame::create(this));
  frame_.Reset(web_view->mainFrame()->toWebLocalFrame());
  node_to_print_.reset();

  // When loading is done this will call didStopLoading() and that will do the
  // actual printing.
  frame()->loadRequest(blink::WebURLRequest(GURL(url_str)));
}

bool PrepareFrameAndViewForPrint::allowsBrokenNullLayerTreeView() const {
  return true;
}

void PrepareFrameAndViewForPrint::didStopLoading() {
  DCHECK(!on_ready_.is_null());
  // Don't call callback here, because it can delete |this| and WebView that is
  // called didStopLoading.
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&PrepareFrameAndViewForPrint::CallOnReady,
                 weak_ptr_factory_.GetWeakPtr()));
}

blink::WebFrame* PrepareFrameAndViewForPrint::createChildFrame(
    blink::WebLocalFrame* parent,
    const blink::WebString& name) {
  blink::WebFrame* frame = blink::WebLocalFrame::create(this);
  parent->appendChild(frame);
  return frame;
}

void PrepareFrameAndViewForPrint::frameDetached(blink::WebFrame* frame) {
  if (frame->parent())
    frame->parent()->removeChild(frame);
  frame->close();
}

void PrepareFrameAndViewForPrint::CallOnReady() {
  return on_ready_.Run();  // Can delete |this|.
}

void PrepareFrameAndViewForPrint::RestoreSize() {
  if (frame()) {
    blink::WebView* web_view = frame_.GetFrame()->view();
    web_view->resize(prev_view_size_);
    if (blink::WebFrame* web_frame = web_view->mainFrame())
      web_frame->setScrollOffset(prev_scroll_offset_);
  }
}

void PrepareFrameAndViewForPrint::FinishPrinting() {
  blink::WebLocalFrame* frame = frame_.GetFrame();
  if (frame) {
    blink::WebView* web_view = frame->view();
    if (is_printing_started_) {
      is_printing_started_ = false;
      frame->printEnd();
      if (!owns_web_view_) {
        web_view->settings()->setShouldPrintBackgrounds(false);
        RestoreSize();
      }
    }
    if (owns_web_view_) {
      DCHECK(!frame->isLoading());
      owns_web_view_ = false;
      web_view->close();
    }
  }
  frame_.Reset(NULL);
  on_ready_.Reset();
}

PrintWebViewHelper::PrintWebViewHelper(content::RenderView* render_view)
    : content::RenderViewObserver(render_view),
      content::RenderViewObserverTracker<PrintWebViewHelper>(render_view),
      reset_prep_frame_view_(false),
      is_print_ready_metafile_sent_(false),
      ignore_css_margins_(false),
      is_scripted_printing_blocked_(false),
      notify_browser_of_print_failure_(true),
      print_for_preview_(false),
      print_node_in_progress_(false),
      is_loading_(false),
      is_scripted_preview_delayed_(false),
      weak_ptr_factory_(this) {
}

PrintWebViewHelper::~PrintWebViewHelper() {}

// Prints |frame| which called window.print().
void PrintWebViewHelper::PrintPage(blink::WebLocalFrame* frame,
                                   bool user_initiated) {
  DCHECK(frame);
  Print(frame, blink::WebNode());
}

bool PrintWebViewHelper::OnMessageReceived(const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(PrintWebViewHelper, message)
    IPC_MESSAGE_HANDLER(PrintMsg_PrintPages, OnPrintPages)
    IPC_MESSAGE_HANDLER(PrintMsg_PrintingDone, OnPrintingDone)
    IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()
  return handled;
}

bool PrintWebViewHelper::GetPrintFrame(blink::WebLocalFrame** frame) {
  DCHECK(frame);
  blink::WebView* webView = render_view()->GetWebView();
  DCHECK(webView);
  if (!webView)
    return false;

  // If the user has selected text in the currently focused frame we print
  // only that frame (this makes print selection work for multiple frames).
  blink::WebLocalFrame* focusedFrame =
      webView->focusedFrame()->toWebLocalFrame();
  *frame = focusedFrame->hasSelection()
               ? focusedFrame
               : webView->mainFrame()->toWebLocalFrame();
  return true;
}

#if !defined(DISABLE_BASIC_PRINTING)
void PrintWebViewHelper::OnPrintPages(bool silent, bool print_background) {
  blink::WebLocalFrame* frame;
  if (GetPrintFrame(&frame))
    Print(frame, blink::WebNode(), silent, print_background);
}
#endif  // !DISABLE_BASIC_PRINTING

void PrintWebViewHelper::GetPageSizeAndContentAreaFromPageLayout(
    const PageSizeMargins& page_layout_in_points,
    gfx::Size* page_size,
    gfx::Rect* content_area) {
  *page_size = gfx::Size(
      page_layout_in_points.content_width +
          page_layout_in_points.margin_right +
          page_layout_in_points.margin_left,
      page_layout_in_points.content_height +
          page_layout_in_points.margin_top +
          page_layout_in_points.margin_bottom);
  *content_area = gfx::Rect(page_layout_in_points.margin_left,
                            page_layout_in_points.margin_top,
                            page_layout_in_points.content_width,
                            page_layout_in_points.content_height);
}

void PrintWebViewHelper::UpdateFrameMarginsCssInfo(
    const base::DictionaryValue& settings) {
  int margins_type = 0;
  if (!settings.GetInteger(kSettingMarginsType, &margins_type))
    margins_type = DEFAULT_MARGINS;
  ignore_css_margins_ = (margins_type != DEFAULT_MARGINS);
}

void PrintWebViewHelper::OnPrintingDone(bool success) {
  notify_browser_of_print_failure_ = false;
  if (!success)
    LOG(ERROR) << "Failure in OnPrintingDone";
  DidFinishPrinting(success ? OK : FAIL_PRINT);
}

void PrintWebViewHelper::PrintNode(const blink::WebNode& node) {
  if (node.isNull() || !node.document().frame()) {
    // This can occur when the context menu refers to an invalid WebNode.
    // See http://crbug.com/100890#c17 for a repro case.
    return;
  }

  if (print_node_in_progress_) {
    // This can happen as a result of processing sync messages when printing
    // from ppapi plugins. It's a rare case, so its OK to just fail here.
    // See http://crbug.com/159165.
    return;
  }

  print_node_in_progress_ = true;
  blink::WebNode duplicate_node(node);
  Print(duplicate_node.document().frame(), duplicate_node);

  print_node_in_progress_ = false;
}

void PrintWebViewHelper::Print(blink::WebLocalFrame* frame,
                               const blink::WebNode& node,
                               bool silent,
                               bool print_background) {
  // If still not finished with earlier print request simply ignore.
  if (prep_frame_view_)
    return;

  FrameReference frame_ref(frame);

  int expected_page_count = 0;
  if (!CalculateNumberOfPages(frame, node, &expected_page_count)) {
    DidFinishPrinting(FAIL_PRINT_INIT);
    return;  // Failed to init print page settings.
  }

  // Some full screen plugins can say they don't want to print.
  if (!expected_page_count) {
    DidFinishPrinting(FAIL_PRINT);
    return;
  }

  // Ask the browser to show UI to retrieve the final print settings.
  if (!silent && !GetPrintSettingsFromUser(frame_ref.GetFrame(), node,
                                           expected_page_count)) {
    DidFinishPrinting(OK);  // Release resources and fail silently.
    return;
  }

  print_pages_params_->params.should_print_backgrounds = print_background;

  // Render Pages for printing.
  if (!RenderPagesForPrint(frame_ref.GetFrame(), node)) {
    LOG(ERROR) << "RenderPagesForPrint failed";
    DidFinishPrinting(FAIL_PRINT);
  }
}

void PrintWebViewHelper::DidFinishPrinting(PrintingResult result) {
  switch (result) {
    case OK:
      break;

    case FAIL_PRINT_INIT:
      DCHECK(!notify_browser_of_print_failure_);
      break;

    case FAIL_PRINT:
      if (notify_browser_of_print_failure_ && print_pages_params_) {
        int cookie = print_pages_params_->params.document_cookie;
        Send(new PrintHostMsg_PrintingFailed(routing_id(), cookie));
      }
      break;
  }
  prep_frame_view_.reset();
  print_pages_params_.reset();
  notify_browser_of_print_failure_ = true;
}

void PrintWebViewHelper::OnFramePreparedForPrintPages() {
  PrintPages();
  FinishFramePrinting();
}

void PrintWebViewHelper::PrintPages() {
  if (!prep_frame_view_)  // Printing is already canceled or failed.
    return;
  prep_frame_view_->StartPrinting();

  int page_count = prep_frame_view_->GetExpectedPageCount();
  if (!page_count) {
    LOG(ERROR) << "Can't print 0 pages.";
    return DidFinishPrinting(FAIL_PRINT);
  }

  const PrintMsg_PrintPages_Params& params = *print_pages_params_;
  const PrintMsg_Print_Params& print_params = params.params;

#if !defined(OS_CHROMEOS) && !defined(OS_ANDROID)
  // TODO(vitalybuka): should be page_count or valid pages from params.pages.
  // See http://crbug.com/161576
  Send(new PrintHostMsg_DidGetPrintedPagesCount(routing_id(),
                                                print_params.document_cookie,
                                                page_count));
#endif  // !defined(OS_CHROMEOS)

  if (!PrintPagesNative(prep_frame_view_->frame(), page_count)) {
    LOG(ERROR) << "Printing failed.";
    return DidFinishPrinting(FAIL_PRINT);
  }
}

void PrintWebViewHelper::FinishFramePrinting() {
  prep_frame_view_.reset();
}

#if defined(OS_MACOSX)
bool PrintWebViewHelper::PrintPagesNative(blink::WebFrame* frame,
                                          int page_count) {
  const PrintMsg_PrintPages_Params& params = *print_pages_params_;
  const PrintMsg_Print_Params& print_params = params.params;

  PrintMsg_PrintPage_Params page_params;
  page_params.params = print_params;
  if (params.pages.empty()) {
    for (int i = 0; i < page_count; ++i) {
      page_params.page_number = i;
      PrintPageInternal(page_params, frame);
    }
  } else {
    for (size_t i = 0; i < params.pages.size(); ++i) {
      if (params.pages[i] >= page_count)
        break;
      page_params.page_number = params.pages[i];
      PrintPageInternal(page_params, frame);
    }
  }
  return true;
}

#endif  // OS_MACOSX

// static - Not anonymous so that platform implementations can use it.
void PrintWebViewHelper::ComputePageLayoutInPointsForCss(
    blink::WebFrame* frame,
    int page_index,
    const PrintMsg_Print_Params& page_params,
    bool ignore_css_margins,
    double* scale_factor,
    PageSizeMargins* page_layout_in_points) {
  PrintMsg_Print_Params params = CalculatePrintParamsForCss(
      frame, page_index, page_params, ignore_css_margins,
      page_params.print_scaling_option ==
          blink::WebPrintScalingOptionFitToPrintableArea,
      scale_factor);
  CalculatePageLayoutFromPrintParams(params, page_layout_in_points);
}

bool PrintWebViewHelper::InitPrintSettings(bool fit_to_paper_size) {
  PrintMsg_PrintPages_Params settings;
  Send(new PrintHostMsg_GetDefaultPrintSettings(routing_id(),
                                                &settings.params));
  // Check if the printer returned any settings, if the settings is empty, we
  // can safely assume there are no printer drivers configured. So we safely
  // terminate.
  bool result = true;
  if (!PrintMsg_Print_Params_IsValid(settings.params))
    result = false;

  // Reset to default values.
  ignore_css_margins_ = false;
  settings.pages.clear();

  settings.params.print_scaling_option =
      blink::WebPrintScalingOptionSourceSize;
  if (fit_to_paper_size) {
    settings.params.print_scaling_option =
        blink::WebPrintScalingOptionFitToPrintableArea;
  }

  SetPrintPagesParams(settings);
  return result;
}

bool PrintWebViewHelper::CalculateNumberOfPages(blink::WebLocalFrame* frame,
                                                const blink::WebNode& node,
                                                int* number_of_pages) {
  DCHECK(frame);
  bool fit_to_paper_size = !(PrintingNodeOrPdfFrame(frame, node));
  if (!InitPrintSettings(fit_to_paper_size)) {
    notify_browser_of_print_failure_ = false;
    Send(new PrintHostMsg_ShowInvalidPrinterSettingsError(routing_id()));
    return false;
  }

  const PrintMsg_Print_Params& params = print_pages_params_->params;
  PrepareFrameAndViewForPrint prepare(params, frame, node, ignore_css_margins_);
  prepare.StartPrinting();

  *number_of_pages = prepare.GetExpectedPageCount();
  return true;
}

bool PrintWebViewHelper::GetPrintSettingsFromUser(blink::WebFrame* frame,
                                                  const blink::WebNode& node,
                                                  int expected_pages_count) {
  PrintHostMsg_ScriptedPrint_Params params;
  PrintMsg_PrintPages_Params print_settings;

  params.cookie = print_pages_params_->params.document_cookie;
  params.has_selection = frame->hasSelection();
  params.expected_pages_count = expected_pages_count;
  MarginType margin_type = DEFAULT_MARGINS;
  if (PrintingNodeOrPdfFrame(frame, node))
    margin_type = GetMarginsForPdf(frame, node);
  params.margin_type = margin_type;

  // PrintHostMsg_ScriptedPrint will reset print_scaling_option, so we save the
  // value before and restore it afterwards.
  blink::WebPrintScalingOption scaling_option =
      print_pages_params_->params.print_scaling_option;

  print_pages_params_.reset();
  IPC::SyncMessage* msg =
      new PrintHostMsg_ScriptedPrint(routing_id(), params, &print_settings);
  msg->EnableMessagePumping();
  Send(msg);
  print_settings.params.print_scaling_option = scaling_option;
  SetPrintPagesParams(print_settings);
  return (print_settings.params.dpi && print_settings.params.document_cookie);
}

bool PrintWebViewHelper::RenderPagesForPrint(blink::WebLocalFrame* frame,
                                             const blink::WebNode& node) {
  if (!frame || prep_frame_view_)
    return false;
  const PrintMsg_PrintPages_Params& params = *print_pages_params_;
  const PrintMsg_Print_Params& print_params = params.params;
  prep_frame_view_.reset(new PrepareFrameAndViewForPrint(
      print_params, frame, node, ignore_css_margins_));
  DCHECK(!print_pages_params_->params.selection_only ||
         print_pages_params_->pages.empty());
  prep_frame_view_->CopySelectionIfNeeded(
      render_view()->GetWebkitPreferences(),
      base::Bind(&PrintWebViewHelper::OnFramePreparedForPrintPages,
                 base::Unretained(this)));
  return true;
}

#if defined(OS_POSIX)
bool PrintWebViewHelper::CopyMetafileDataToSharedMem(
    PdfMetafileSkia* metafile,
    base::SharedMemoryHandle* shared_mem_handle) {
  uint32 buf_size = metafile->GetDataSize();
  scoped_ptr<base::SharedMemory> shared_buf(
      content::RenderThread::Get()->HostAllocateSharedMemoryBuffer(
          buf_size).release());

  if (shared_buf) {
    if (shared_buf->Map(buf_size)) {
      metafile->GetData(shared_buf->memory(), buf_size);
      return shared_buf->GiveToProcess(base::GetCurrentProcessHandle(),
                                       shared_mem_handle);
    }
  }
  return false;
}
#endif  // defined(OS_POSIX)

void PrintWebViewHelper::SetPrintPagesParams(
    const PrintMsg_PrintPages_Params& settings) {
  print_pages_params_.reset(new PrintMsg_PrintPages_Params(settings));
}

}  // namespace printing
