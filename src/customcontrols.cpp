/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2015-2025 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "customcontrols.h"

#include "concurrency.h"
#include "errors.h"
#include "hidpi.h"
#include "utility.h"
#include "str_helpers.h"
#include "unicode_helpers.h"

#include <wx/activityindicator.h>
#include <wx/app.h>
#include <wx/artprov.h>
#include <wx/clipbrd.h>
#include <wx/dcmemory.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/renderer.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#ifdef __WXMSW__
#include <wx/generic/private/markuptext.h>
#endif

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <map>
#include <memory>

namespace
{

wxString WrapTextAtWidth(const wxString& text_, int width, Language lang, wxWindow *wnd)
{
    if (text_.empty())
        return text_;

#ifdef BIDI_NEEDS_DIRECTION_ON_EACH_LINE
    wchar_t directionMark = 0;
    if (bidi::is_direction_mark(*text_.begin()))
        directionMark = *text_.begin();
#endif
        
    auto text = str::to_icu(text_);

    static std::map<std::string, std::shared_ptr<unicode::BreakIterator>> lang_iters;
    std::shared_ptr<unicode::BreakIterator> iter;
    auto lang_name = lang.IcuLocaleName();
    auto li = lang_iters.find(lang_name);
    if (li == lang_iters.end())
    {
        iter.reset(new unicode::BreakIterator(UBRK_LINE, lang));
        lang_iters[lang_name] = iter;
    }
    else
    {
        iter = li->second;
    }

    iter->set_text(text);

    wxString out;
    out.reserve(text_.length() + 10);

    int32_t lineStart = 0;
    wxString previousSubstr;

    for (int32_t pos = iter->begin(); pos != iter->end(); pos = iter->next())
    {
        if (pos == lineStart)
            continue;
        auto substr = str::to_wx(text + lineStart, pos - lineStart);
        substr.Trim();

        if (wnd->GetTextExtent(substr).x > width)
        {
            auto previousPos = iter->previous();
            if (previousPos == lineStart || previousPos == iter->end())
            {
                // line is too large but we can't break it, so have no choice but not to wrap
                out += substr;
                lineStart = pos;
            }
            else
            {
                // need to wrap at previous linebreak position
                out += previousSubstr;
                lineStart = previousPos;
            }

            out += '\n';
#ifdef BIDI_NEEDS_DIRECTION_ON_EACH_LINE
            if (directionMark)
                out += directionMark;
#endif

            previousSubstr.clear();
        }
        else if (pos > 0 && text[pos-1] == '\n') // forced line feed
        {
            out += substr;
            out += '\n';
#ifdef BIDI_NEEDS_DIRECTION_ON_EACH_LINE
            if (directionMark)
                out += directionMark;
#endif
            lineStart = pos;
            previousSubstr.clear();
        }
        else
        {
            previousSubstr = substr;
        }
    }

    if (!previousSubstr.empty())
    {
        out += previousSubstr;
    }

    out.Trim();

    return out;
}


} // anonymous namespace


HeadingLabel::HeadingLabel(wxWindow *parent, const wxString& label)
    : wxStaticText(parent, wxID_ANY, label)
{
#ifdef __WXGTK3__
    // This is needed to avoid missizing text with bold font. See
    // https://github.com/vslavik/poedit/pull/411 and https://trac.wxwidgets.org/ticket/16088
    SetLabelMarkup("<b>" + EscapeMarkup(label) + "</b>");
#else
    SetFont(GetFont().Bold());
#endif
}


AutoWrappingText::AutoWrappingText(wxWindow *parent, wxWindowID winid, const wxString& label)
    : wxStaticText(parent, winid, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE),
      m_text(label),
      m_wrapWidth(-1)
{
    SetMinSize(wxDefaultSize);
    Bind(wxEVT_SIZE, &AutoWrappingText::OnSize, this);
}

void AutoWrappingText::SetLabel(const wxString& label) {
    wxString escapedLabel(label);

    // Escape '&' to avoid wxStaticText treating it
    // as if to mark an accelerator key (Windows),
    // or not showing it at all (Mac).
    escapedLabel.Replace("&", "&&");
    wxStaticText::SetLabel(escapedLabel);
}

void AutoWrappingText::SetLanguage(Language lang)
{
    m_language = lang;
    SetAlignment(m_language.Direction());
}

void AutoWrappingText::SetAlignment(TextDirection dir)
{
    // a quirk of wx API: if the current locale is RTL, the meaning of L and R is reversed
    // for alignments
    bool isRTL = (dir == TextDirection::RTL);
    if (GetLayoutDirection() == wxLayout_RightToLeft)
        isRTL = !isRTL;

    const int align = isRTL ? wxALIGN_RIGHT : wxALIGN_LEFT;
    if (HasFlag(align))
        return;
    SetWindowStyleFlag(wxST_NO_AUTORESIZE | align);
}

void AutoWrappingText::SetAndWrapLabel(const wxString& label)
{
    m_text = bidi::platform_mark_direction(label);
    if (!m_language.IsValid())
        SetAlignment(bidi::get_base_direction(m_text));

    m_wrapWidth = -1; // force rewrap

    RewrapForWidth(GetSize().x);
}

bool AutoWrappingText::InformFirstDirection(int direction, int size, int /*availableOtherDir*/)
{
    if (size > 0 && direction == wxHORIZONTAL)
        return RewrapForWidth(size);
    return false;
}

#ifdef __WXOSX__
wxSize AutoWrappingText::DoGetBestSize() const
{
    auto sz = wxStaticText::DoGetBestSize();
    // AppKit's intristicContentSize calculation is sometimes subtly wrong in our use case,
    // hiding the last line of wrapped text. It seems to be off-by-two error only:
    if (sz.y > 0)
        sz.y += 2;
    return sz;
}
#endif

void AutoWrappingText::OnSize(wxSizeEvent& e)
{
    e.Skip();
    RewrapForWidth(e.GetSize().x);
}

bool AutoWrappingText::RewrapForWidth(int width)
{
    if (width == m_wrapWidth)
        return false;

    // refuse to participate in crazy-small sizes sizing (will be undone anyway):
    if (width < 50)
        return false;

    m_wrapWidth = width;

    const int wrapAt = wxMax(0, width - PX(4));
    wxWindowUpdateLocker lock(this);
    SetLabel(WrapTextAtWidth(m_text, wrapAt, m_language, this));

    InvalidateBestSize();
    return true;
}


SelectableAutoWrappingText::SelectableAutoWrappingText(wxWindow *parent, wxWindowID winid, const wxString& label)
    : AutoWrappingText(parent, winid, label)
{
#if defined(__WXOSX__)
    NSTextField *view = (NSTextField*)GetHandle();
    [view setSelectable:YES];
#elif defined(__WXGTK__)
    GtkLabel *view = GTK_LABEL(GetHandle());
    gtk_label_set_selectable(view, TRUE);
#else
    // at least allow copying
    static wxWindowIDRef idCopy = NewControlId();
    Bind(wxEVT_CONTEXT_MENU, [=](wxContextMenuEvent&){
        wxMenu menu;
        menu.Append(idCopy, _("&Copy"));
        PopupMenu(&menu);
    });
    Bind(wxEVT_MENU, [=](wxCommandEvent&){
        wxClipboardLocker lock;
        wxClipboard::Get()->SetData(new wxTextDataObject(m_text));
    }, idCopy);
#endif
}


ExplanationLabel::ExplanationLabel(wxWindow *parent, const wxString& label)
    : AutoWrappingText(parent, wxID_ANY, label)
{
#if defined(__WXOSX__) || defined(__WXGTK__)
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
#ifndef __WXGTK__
    ColorScheme::SetupWindowColors(this, [=]
    {
        SetForegroundColour(GetTextColor());
    });
#endif
}


SecondaryLabel::SecondaryLabel(wxWindow *parent, const wxString& label)
    : wxStaticText(parent, wxID_ANY, label)
{
#if defined(__WXOSX__) || defined(__WXGTK__)
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
#ifndef __WXGTK__
    ColorScheme::SetupWindowColors(this, [=]
    {
        SetForegroundColour(GetTextColor());
    });
#endif
}


LearnMoreLink::LearnMoreLink(wxWindow *parent, const wxString& url, wxString label, wxWindowID winid)
{
    if (label.empty())
        label = _("Learn more");

    wxHyperlinkCtrl::Create(parent, winid, label, url);

    ColorScheme::SetupWindowColors(this, [=]
    {
#ifdef __WXOSX__
        wxColour normal, hover;
        NSView *view = GetHandle();
        // FIXME: This is workaround for wx always overriding appearance to the app-wide system one when
        //        accessing wxColour components or creating CGColor -- as happens in generic rendering code
        //        (see wxOSXEffectiveAppearanceSetter)
        [view.effectiveAppearance performAsCurrentDrawingAppearance: [&]{
            normal = wxColour(wxCFRetain([NSColor.linkColor CGColor]));
            hover = wxColour(wxCFRetain([[NSColor.linkColor colorWithSystemEffect:NSColorSystemEffectRollover] CGColor]));
        }];

        SetNormalColour(normal);
        SetVisitedColour(normal);
        SetHoverColour(hover);
#else
        SetNormalColour("#2F79BE");
        SetVisitedColour("#2F79BE");
        SetHoverColour("#3D8DD5");
#endif
    });

#ifdef __WXOSX__
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    SetFont(GetFont().Underlined());
#endif
}


wxObject *LearnMoreLinkXmlHandler::DoCreateResource()
{
    auto w = new LearnMoreLink(m_parentAsWindow, GetText("url"), GetText("label"), GetID());
    w->SetName(GetName());
    SetupWindow(w);
    return w;
}

bool LearnMoreLinkXmlHandler::CanHandle(wxXmlNode *node)
{
    return IsOfClass(node, "LearnMoreLink");
}


ActivityIndicator::ActivityIndicator(wxWindow *parent, int flags)
    : wxWindow(parent, wxID_ANY), m_running(false)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    SetSizer(sizer);

    m_spinner = new wxActivityIndicator(this, wxID_ANY);
    m_spinner->Hide();
    m_spinner->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_label = new wxStaticText(this, wxID_ANY, "");
#ifdef __WXOSX__
    SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_label->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif

    if (flags & Centered)
        sizer->AddStretchSpacer();
    sizer->Add(m_spinner, wxSizerFlags().Center().Border(wxRIGHT, PX(4)));
    sizer->Add(m_label, wxSizerFlags().Center());
    if (flags & Centered)
        sizer->AddStretchSpacer();

    wxWeakRef<ActivityIndicator> self(this);
    HandleError = [self](dispatch::exception_ptr e){
        dispatch::on_main([self,e]{
            if (self)
                self->StopWithError(DescribeException(e));
        });
    };
}


void ActivityIndicator::UpdateLayoutAfterTextChange()
{
    m_label->Wrap(GetSize().x);

    Layout();

    if (GetSizer()->IsShown(m_label))
    {
        InvalidateBestSize();
        SetMinSize(wxDefaultSize);
        SetMinSize(GetBestSize());

        GetParent()->Layout();
    }
}


void ActivityIndicator::Start(const wxString& msg)
{
    m_running = true;

    m_label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    m_label->SetLabel(msg);

    auto sizer = GetSizer();
    sizer->Show(m_spinner);
    sizer->Show(m_label, !msg.empty());

    UpdateLayoutAfterTextChange();

    m_spinner->Start();
}

void ActivityIndicator::Stop()
{
    m_running = false;

    m_spinner->Stop();
    m_label->SetLabel("");

    auto sizer = GetSizer();
    sizer->Hide(m_spinner);
    sizer->Hide(m_label);

    UpdateLayoutAfterTextChange();
}

void ActivityIndicator::StopWithError(const wxString& msg)
{
    m_running = false;

    m_spinner->Stop();
    m_label->SetForegroundColour(ColorScheme::Get(Color::ErrorText));
    m_label->SetLabel(msg);
    m_label->SetToolTip(msg);

    auto sizer = GetSizer();
    sizer->Hide(m_spinner);
    sizer->Show(m_label);

    UpdateLayoutAfterTextChange();
}



ImageButton::ImageButton(wxWindow *parent, const wxString& bitmapName)
    : wxBitmapButton(parent, wxID_ANY,
                     bitmapName.empty() ? wxNullBitmap : wxArtProvider::GetBitmap(bitmapName),
                     wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT),
      m_bitmapName(bitmapName)
{
#ifdef __WXOSX__
    // don't light up the background when clicked:
    NSButton *view = (NSButton*)GetHandle();
    view.buttonType = NSButtonTypeMomentaryChange;
#else
    // refresh template icons on theme change (macOS handles automatically):
    if (bitmapName.ends_with("Template"))
    {
        ColorScheme::SetupWindowColors(this, [=]
        {
            SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
        });
    }
#endif
}


StaticBitmap::StaticBitmap(wxWindow *parent, const wxString& bitmapName)
    : wxStaticBitmap(parent, wxID_ANY,
                     bitmapName.empty() ? wxNullBitmap : wxArtProvider::GetBitmap(bitmapName)),
      m_bitmapName(bitmapName)
{
#ifndef __WXOSX__
    // refresh template icons on theme change (macOS handles automatically):
    if (bitmapName.ends_with("Template"))
    {
        ColorScheme::SetupWindowColors(this, [=]
        {
            SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
        });
    }
#endif
}

void StaticBitmap::SetBitmapName(const wxString& bitmapName)
{
    m_bitmapName = bitmapName;
    SetBitmap(wxArtProvider::GetBitmap(m_bitmapName));
}


AvatarIcon::AvatarIcon(wxWindow *parent, const wxSize& size) : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
{
    InitForSize();
    ColorScheme::RefreshOnChange(this);

    Bind(wxEVT_PAINT, &AvatarIcon::OnPaint, this);
}

void AvatarIcon::SetUserName(const wxString& name)
{
    m_placeholder.clear();
    for (auto& s: wxSplit(name, ' '))
    {
        if (!s.empty())
            m_placeholder += s[0];
    }
    Refresh();
}

void AvatarIcon::LoadIcon(const wxFileName& f)
{
#ifdef __WXOSX__
    NSString *path = str::to_NS(f.GetFullPath());
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:path];
    if (img != nil)
        m_bitmap = wxBitmap(img);
#else
    wxLogNull null;
    wxImage img(f.GetFullPath());
    if (img.IsOk())
        m_bitmap = wxBitmap(img);
#endif

    Refresh();
}

void AvatarIcon::InitForSize()
{
    auto size = GetSize();
    wxBitmap bmp(size);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetBrush(*wxBLACK_BRUSH);
    dc.SetPen(*wxBLACK_PEN);
    wxRect r(wxPoint(0,0), size);
    r.Deflate(PX(3));
    dc.DrawEllipse(r);
    dc.SelectObject(wxNullBitmap);
    m_clipping = wxRegion(bmp, *wxWHITE);

    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetWeight(wxFONTWEIGHT_BOLD);
    font.SetPixelSize(wxSize(0, size.y / 4));
    SetFont(font);
}

void AvatarIcon::OnPaint(wxPaintEvent&)
{
    auto r = GetClientRect();
    r.Deflate(PX(2));

    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetInterpolationQuality(wxINTERPOLATION_BEST);

    gc->Clip(m_clipping);

    if (m_bitmap.IsOk())
    {
        gc->DrawBitmap(m_bitmap, r.x, r.y, r.width, r.height);
    }
    else
    {
        gc->SetBrush(wxColour(128,128,128,50));
        gc->SetPen(wxNullPen);
        gc->SetFont(GetFont(), ColorScheme::Get(Color::SecondaryLabel));

        gc->DrawEllipse(r.x, r.y, r.width, r.height);

        wxDouble tw, th;
        gc->GetTextExtent(m_placeholder, &tw, &th);
        gc->DrawText(m_placeholder, r.x + (r.width - tw) / 2, r.y + (r.height - th) / 2);
    }

    gc->ResetClip();

    // mark out jagged, pixelated clipping due to low-resolution wxRegion:
    auto outline = GetBackgroundColour();
    outline = outline.ChangeLightness(ColorScheme::GetAppMode() == ColorScheme::Light ? 98 : 110);
    gc->SetPen(wxPen(outline, PX(2)));
    gc->DrawEllipse(r.x + 0.5, r.y + 0.5, r.width, r.height);
}



class IconAndSubtitleListCtrl::MultilineTextRenderer : public wxDataViewTextRenderer
{
public:
    MultilineTextRenderer() : wxDataViewTextRenderer()
    {
        EnableMarkup();
    }

#ifdef __WXMSW__
    bool Render(wxRect rect, wxDC *dc, int state)
    {
        int flags = 0;
        if ( state & wxDATAVIEW_CELL_SELECTED )
            flags |= wxCONTROL_SELECTED;

        rect.height /= 2;
        for (auto& line: wxSplit(m_text, '\n'))
        {
            wxItemMarkupText markup(line);
            markup.Render(GetView(), *dc, rect, flags, GetEllipsizeMode());
            rect.y += rect.height;
        }

        return true;
    }

    wxSize GetSize() const
    {
        if (m_text.empty())
            return wxSize(wxDVC_DEFAULT_RENDERER_SIZE,wxDVC_DEFAULT_RENDERER_SIZE);

        auto size = wxDataViewTextRenderer::GetSize();
        size.y *= 2; // approximation enough for our needs
        return size;
    }
#endif // __WXMSW__
};


// TODO: merge with CloudFileList which is very similar and has lot of duplicated code
IconAndSubtitleListCtrl::IconAndSubtitleListCtrl(wxWindow *parent, const wxString& columnTitle, long style)
    : wxDataViewListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_NO_HEADER | style)
{
#ifdef __WXOSX__
    NSScrollView *scrollView = (NSScrollView*)GetHandle();
    NSTableView *tableView = (NSTableView*)[scrollView documentView];
    tableView.intercellSpacing = NSMakeSize(0.0, 0.0);
    tableView.style = NSTableViewStyleFullWidth;

    const int icon_column_width = PX(32 + 12);
#else // !__WXOSX__
    const int icon_column_width = wxSystemSettings::GetMetric(wxSYS_ICON_X) + PX(12);
#endif

    SetRowHeight(GetDefaultRowHeight());

    AppendBitmapColumn("", 0, wxDATAVIEW_CELL_INERT, icon_column_width);
    auto renderer = new MultilineTextRenderer();
    auto column = new wxDataViewColumn(columnTitle, renderer, 1, -1, wxALIGN_NOT, wxDATAVIEW_COL_RESIZABLE);
    AppendColumn(column, "string");

#ifndef __WXGTK__
    ColorScheme::SetupWindowColors(this, [=]{ OnColorChange(); });
#endif
}

int IconAndSubtitleListCtrl::GetDefaultRowHeight() const
{
    return PX(46);
}

wxString IconAndSubtitleListCtrl::FormatItemText(const wxString& title, const wxString& description)
{
#ifdef __WXGTK__
    auto secondaryFormatting = "alpha='50%'";
#else
    auto secondaryFormatting = GetSecondaryFormatting();
#endif

    return wxString::Format
    (
        "%s\n<small><span %s>%s</span></small>",
        EscapeMarkup(title),
        secondaryFormatting,
        EscapeMarkup(description)
    );
}

#ifndef __WXGTK__
wxString IconAndSubtitleListCtrl::GetSecondaryFormatting()
{
    auto secondaryFormatting = wxString::Format("foreground='%s'", ColorScheme::Get(Color::SecondaryLabel).GetAsString(wxC2S_HTML_SYNTAX));
    m_secondaryFormatting[ColorScheme::GetAppMode()] = secondaryFormatting;
    return secondaryFormatting;
}

void IconAndSubtitleListCtrl::OnColorChange()
{
    auto otherMode = (ColorScheme::GetAppMode() == ColorScheme::Light) ? ColorScheme::Dark : ColorScheme::Light;
    auto repl_from = m_secondaryFormatting[otherMode];
    auto repl_to = GetSecondaryFormatting();
    if (repl_from.empty() || repl_to.empty())
        return;

    for (auto row = 0; row < GetItemCount(); row++)
    {
        auto text = GetTextValue(row, 1);
        text.Replace(repl_from, repl_to);
        SetTextValue(text, row, 1);
    }
}
#endif // !__WXGTK__

void IconAndSubtitleListCtrl::AppendFormattedItem(const wxBitmap& icon, const wxString& title, const wxString& description)
{
    wxVector<wxVariant> data;
    data.push_back(wxVariant(icon));
    data.push_back(FormatItemText(title, description));
    AppendItem(data);
}

void IconAndSubtitleListCtrl::UpdateFormattedItem(unsigned row, const wxString& title, const wxString& description)
{
    SetTextValue(FormatItemText(title, description), row, 1);
}
