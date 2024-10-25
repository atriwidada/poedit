/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 1999-2024 Vaclav Slavik
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

#include "editing_area.h"

#include "colorscheme.h"
#include "custom_buttons.h"
#include "customcontrols.h"
#include "custom_notebook.h"
#include "edlistctrl.h"
#include "hidpi.h"
#include "spellchecking.h"
#include "static_ids.h"
#include "text_control.h"
#include "utility.h"

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>


namespace
{

struct EventHandlerDisabler
{
    EventHandlerDisabler(wxEvtHandler *h) : m_hnd(h)
        { m_hnd->SetEvtHandlerEnabled(false); }
    ~EventHandlerDisabler()
        { m_hnd->SetEvtHandlerEnabled(true); }

    wxEvtHandler *m_hnd;
};

void SetTranslationValue(TranslationTextCtrl *txt, const wxString& value, int flags)
{
    // disable EVT_TEXT forwarding -- the event is generated by
    // programmatic changes to text controls' content and we *don't*
    // want UpdateFromTextCtrl() to be called from here
    EventHandlerDisabler disabler(txt->GetEventHandler());

    if (flags & EditingArea::UndoableEdit)
        txt->SetPlainTextUserWritten(value);
    else
        txt->SetPlainText(value);
}

inline void SetCtrlFont(wxWindow *win, const wxFont& font)
{
    if (!win)
        return;

#ifdef __WXMSW__
    // Native wxMSW text control sends EN_CHANGE when the font changes,
    // producing a wxEVT_TEXT event as if the user changed the value.
    // Unfortunately the event seems to be used internally for sizing,
    // so we can't just filter it out completely. What we can do, however,
    // is to disable *our* handling of the event.
    EventHandlerDisabler disabler(win->GetEventHandler());
#endif
    win->SetFont(font);
}

// does some basic processing of user input, e.g. to remove trailing \n
wxString PreprocessEnteredTextForItem(CatalogItemPtr item, wxString t)
{
    auto& orig = item->GetString();

    if (!t.empty() && !orig.empty())
    {
        if (orig.Last() == '\n' && t.Last() != '\n')
            t.append(1, '\n');
        else if (orig.Last() != '\n' && t.Last() == '\n')
            t.RemoveLast();
    }

    return t;
}


/// Box sizer that allows one element to shrink below min size,
class ShrinkableBoxSizer : public wxBoxSizer
{
public:
    ShrinkableBoxSizer(int orient) : wxBoxSizer(orient) {}

    void SetShrinkableWindow(wxWindow *win)
    {
        m_shrinkable = win ? GetItem(win) : nullptr;
    }

    void RepositionChildren(const wxSize& minSize) override
    {
        if (m_shrinkable)
        {
            const wxCoord totalSize = GetSizeInMajorDir(m_size);
            const wxCoord minMSize = GetSizeInMajorDir(minSize);
            // If there's not enough space, make shrinkable item proportional,
            // it will be resized under its minimal size then.
            m_shrinkable->SetProportion(totalSize > 20 && totalSize < minMSize ? 10000 : 0);
        }

        wxBoxSizer::RepositionChildren(minSize);
    }

private:
    wxSizerItem *m_shrinkable;
};


// Pretifies c-format etc. tags. Use canonical spelling for known languages,
// fall back to upper-casing only the first letter.
wxString PrettyPrintFormatTag(const wxString& fmt)
{
    if (fmt.empty())
        return fmt;
    else if (fmt == "php")
        return "PHP";
    else if (fmt == "csharp")
        return "C#";
    else if (fmt == "objc")
        return "Objective-C";
    else if (fmt == "sh")
        return "Shell";
    else if (fmt == "kde")
        return "KDE";
    else if (fmt == "javascript")
        return "JavaScript";
    else if (fmt == "qt" || fmt == "qt-plural")
        return "Qt";
    else if (fmt == "kde" || fmt == "kde-kuit")
        return "KDE";
    else if (fmt == "python-brace")
        return "Python";
    else if (fmt == "perl-brace")
        return "Perl";
    else if (fmt == "object-pascal")
        return "Pascal";
    else
        return wxToupper(fmt[0]) + fmt.substr(1);
}

} // anonymous namespace


/// Tag-like label, with background rounded rect
class EditingArea::TagLabel : public wxWindow
{
public:
    enum Mode
    {
        Fixed,
        Ellipsize
    };

    TagLabel(wxWindow *parent, Color fg, Color bg, wxWindowID labelChildID = wxID_ANY) : wxWindow(parent, wxID_ANY)
    {
        m_icon = nullptr;

        m_label = new wxStaticText(this, labelChildID, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
#ifdef __WXOSX__
        m_label->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_label, wxSizerFlags(1).Center().Border(wxALL, PX(2)));
#ifdef __WXMSW__
        sizer->InsertSpacer(0, PX(2));
        sizer->AddSpacer(PX(2));
#endif
        SetSizer(sizer);

        Bind(wxEVT_PAINT, &TagLabel::OnPaint, this);

        SetColor(fg, bg);

        ColorScheme::SetupWindowColors(this, [=]
        {
        #ifdef __WXMSW__
            SetBackgroundColour(ColorScheme::Get(Color::EditingThickSeparator));
        #endif
            UpdateColor();
        });
    }

    void SetLabel(const wxString& text) override
    {
        m_label->SetLabel(text);
        InvalidateBestSize();
    }

    void SetColor(Color fg, Color bg)
    {
        m_fgSym = fg;
        m_bgSym = bg;
        UpdateColor();
    }

    void SetIcon(const wxBitmap& icon)
    {
        auto sizer = GetSizer();
        if (icon.IsOk())
        {
            if (!m_icon)
            {
                m_icon = new wxStaticBitmap(this, wxID_ANY, icon);
#ifdef __WXMSW__
                ColorScheme::SetupWindowColors(m_icon, [=]{ m_icon->SetBackgroundColour(m_bg); });
#endif
                sizer->Insert(0, m_icon, wxSizerFlags().Center().Border(wxLEFT, PX(2)));
            }
            m_icon->SetBitmap(icon);
            sizer->Show(m_icon);
        }
        else
        {
            if (m_icon)
                sizer->Hide(m_icon);
        }
    }

protected:
    void UpdateColor()
    {
        m_fg = ColorScheme::GetBlendedOn(m_fgSym, this, m_bgSym);
        m_bg = ColorScheme::GetBlendedOn(m_bgSym, this);

        m_label->SetForegroundColour(m_fg);
#ifdef __WXMSW__
        for (auto c : GetChildren())
            c->SetBackgroundColour(m_bg);
#endif
    }

    void DoSetToolTipText(const wxString &tip) override
    {
        wxWindow::DoSetToolTipText(tip);
        m_label->SetToolTip(tip);
    }

#ifdef __WXOSX__
    wxSize DoGetBestSize() const override
    {
        auto size = wxWindow::DoGetBestSize();
        size.y = std::max(20, size.y);
        return size;
    }
#endif

protected:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        gc->SetBrush(m_bg);
        gc->SetPen(*wxTRANSPARENT_PEN);

        auto rect = GetClientRect();
        if (!rect.IsEmpty())
        {
            gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, PX(2));
        }
    }

    Color m_fgSym, m_bgSym;
    wxColour m_fg, m_bg;
    wxStaticText *m_label;
    wxStaticBitmap *m_icon;
};


class EditingArea::IssueLabel : public EditingArea::TagLabel
{
public:
    IssueLabel(wxWindow *parent)
        : TagLabel(parent, Color::TagErrorLineFg, Color::TagErrorLineBg, WinID::TranslationIssueText)
    {
        m_iconError = wxArtProvider::GetBitmap("StatusErrorBlack");
        m_iconWarning = wxArtProvider::GetBitmap("StatusWarningBlack");
        SetIcon(m_iconError);
    }

    std::shared_ptr<CatalogItem::Issue> GetIssue() const { return m_issue; }

    void SetIssue(const std::shared_ptr<CatalogItem::Issue>& issue)
    {
        m_issue = issue;
        switch (issue->severity)
        {
            case CatalogItem::Issue::Error:
                SetIcon(m_iconError);
                SetColor(Color::TagErrorLineFg, Color::TagErrorLineBg);
                break;
            case CatalogItem::Issue::Warning:
                SetIcon(m_iconWarning);
                SetColor(Color::TagWarningLineFg, Color::TagWarningLineBg);
                break;
        }
        SetLabel(issue->message);
        SetToolTip(issue->message);
    }

protected:

    std::shared_ptr<CatalogItem::Issue> m_issue;
    wxBitmap m_iconError, m_iconWarning;
};


class EditingArea::CharCounter : public SecondaryLabel
{
public:
    CharCounter(wxWindow *parent, Mode mode) : SecondaryLabel(parent, "MMMM | MMMM"), m_mode(mode)
    {
        SetWindowStyleFlag(wxALIGN_RIGHT | wxST_NO_AUTORESIZE);

        switch (mode)
        {
            case Editing:
                SetToolTip(_("String length in characters: translation | source"));
                break;
            case POT:
                SetToolTip(_("String length in characters"));
                break;
        }
    }

    void UpdateSourceLength(int i) { m_source = i; UpdateText(); }
    void UpdateTranslationLength(int i) { m_translation = i; UpdateText(); }

private:
    void UpdateText()
    {
        if (m_mode == Editing)
            SetLabel(wxString::Format("%d | %d", m_translation, m_source));
        else
            SetLabel(wxString::Format("%d", m_source));
    }

    Mode m_mode;
    int m_source = 0, m_translation = 0;
};


EditingArea::EditingArea(wxWindow *parent, PoeditListCtrl *associatedList, Mode mode)
    : m_associatedList(associatedList),
      m_dontAutoclearFuzzyStatus(false),
      m_textOrig(nullptr),
      m_textOrigPlural(nullptr),
      m_fuzzy(nullptr),
      m_textTrans(nullptr),
      m_pluralNotebook(nullptr),
      m_labelSingular(nullptr),
      m_labelPlural(nullptr),
      m_labelSource(nullptr),
      m_labelTrans(nullptr),
      m_tagIdOrContext(nullptr),
      m_tagFormat(nullptr),
      m_tagPretranslated(nullptr),
      m_issueLine(nullptr)
{
    wxPanel::Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                    wxTAB_TRAVERSAL | wxNO_BORDER | wxFULL_REPAINT_ON_RESIZE);
#ifdef __WXMSW__
    SetDoubleBuffered(true);
#endif

    Bind(wxEVT_PAINT, &EditingArea::OnPaint, this);

    m_labelSource = new wxStaticText(this, -1, _("Source text"));
#ifdef __WXOSX__
    m_labelSource->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    m_labelSource->SetFont(m_labelSource->GetFont().Bold());

    m_tagIdOrContext = new TagLabel(this, Color::TagContextFg, Color::TagContextBg);
    m_tagFormat = new TagLabel(this, Color::TagSecondaryFg, Color::TagSecondaryBg);

    m_charCounter = new CharCounter(this, mode);

    auto sourceLineSizer = new ShrinkableBoxSizer(wxHORIZONTAL);
    sourceLineSizer->Add(m_labelSource, wxSizerFlags().Center());
    sourceLineSizer->AddSpacer(PX(4));
    sourceLineSizer->Add(m_tagIdOrContext, wxSizerFlags().Center().Border(wxRIGHT, PX(4)));
    sourceLineSizer->Add(m_tagFormat, wxSizerFlags().Center().Border(wxRIGHT, PX(4)));
    sourceLineSizer->AddStretchSpacer(1);
    sourceLineSizer->Add(m_charCounter, wxSizerFlags().Center());
    sourceLineSizer->AddSpacer(PX(4));
    sourceLineSizer->SetShrinkableWindow(m_tagIdOrContext);
    sourceLineSizer->SetMinSize(-1, m_tagIdOrContext->GetBestSize().y);

    m_labelSingular = new wxStaticText(this, -1, _("Singular"));
    m_labelSingular->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_labelSingular->SetFont(m_labelSingular->GetFont().Bold());
    m_textOrig = new SourceTextCtrl(this, wxID_ANY);

    m_labelPlural = new wxStaticText(this, -1, _("Plural"));
    m_labelPlural->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
    m_labelPlural->SetFont(m_labelPlural->GetFont().Bold());
    m_textOrigPlural = new SourceTextCtrl(this, wxID_ANY);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer);

#if defined(__WXMSW__)
    sizer->AddSpacer(PX(4) - 4); // account for fixed 4px sash above
#elif defined(__WXOSX__)
    sizer->AddSpacer(PX(2));
#endif
    sizer->Add(sourceLineSizer, wxSizerFlags().Expand().Border(wxLEFT, PX(5)));
    sizer->AddSpacer(PX(6));

    auto origTextSizer = new wxBoxSizer(wxVERTICAL);
    origTextSizer->AddSpacer(PX(4));
    origTextSizer->Add(m_labelSingular, wxSizerFlags().Border(wxLEFT, PX(5)));
    origTextSizer->Add(m_textOrig, wxSizerFlags(1).Expand());
    origTextSizer->Add(m_labelPlural, wxSizerFlags().Border(wxLEFT, PX(5)));
    origTextSizer->Add(m_textOrigPlural, wxSizerFlags(1).Expand());
    sizer->Add(origTextSizer, wxSizerFlags(1).Expand());

    if (mode == POT)
        CreateTemplateControls(sizer);
    else
        CreateEditControls(sizer);

    SetupTextCtrlSizes();

    ColorScheme::SetupWindowColors(this, [=]
    {
        SetBackgroundColour(ColorScheme::Get(Color::EditingBackground));
    #ifdef __WXMSW__
        m_labelSource->SetBackgroundColour(ColorScheme::Get(Color::EditingThickSeparator));
        m_charCounter->SetBackgroundColour(ColorScheme::Get(Color::EditingThickSeparator));
    #endif
        m_labelSingular->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
        m_labelPlural->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
    });
}


void EditingArea::CreateEditControls(wxBoxSizer *sizer)
{
    m_labelTrans = new wxStaticText(this, -1, _("Translation"));
#ifdef __WXOSX__
    m_labelTrans->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    m_labelTrans->SetFont(m_labelTrans->GetFont().Bold());

    m_issueLine = new IssueLabel(this);

    m_tagPretranslated = new TagLabel(this, Color::TagSecondaryFg, Color::TagSecondaryBg);
    m_tagPretranslated->SetLabel(_("Pre-translated"));

    auto transLineSizer = new ShrinkableBoxSizer(wxHORIZONTAL);
    transLineSizer->Add(m_labelTrans, wxSizerFlags().Center());
    transLineSizer->AddSpacer(PX(4));
    transLineSizer->Add(m_issueLine, wxSizerFlags().Center().Border(wxRIGHT, PX(4)));
    transLineSizer->SetShrinkableWindow(m_issueLine);

    transLineSizer->AddStretchSpacer(1);
    transLineSizer->Add(m_tagPretranslated, wxSizerFlags().Center().Border(wxRIGHT, 3*PX(4)));

#ifndef __WXOSX__
    transLineSizer->SetMinSize(-1, m_issueLine->GetBestSize().y);
#endif

    // TRANSLATORS: This indicates that the string's translation isn't final
    // and has known problems.  For example, it might be machine translated or
    // fuzzy matched from an older string. The translation should be short and
    // convey this. If it's problematic to translate it, "Needs review" is
    // acceptable substitute, but note that the meaning is subtly different:
    // "needs review" implies that somebody else should review the string after
    // I am done with it (i.e. consider it good), while "needs work" implies I
    // need to return to it and finish the translation.
    m_fuzzy = new SwitchButton(this, WinID::NeedsWorkSwitch, MSW_OR_OTHER(_("Needs work"), _("Needs Work")));
#ifdef __WXOSX__
    m_fuzzy->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif
    transLineSizer->Add(m_fuzzy, wxSizerFlags().Center().Border(wxTOP, MSW_OR_OTHER(IsHiDPI() ? PX(1) : 0, 0)));
    transLineSizer->AddSpacer(PX(4));

    m_textTrans = new TranslationTextCtrl(this, wxID_ANY);

    // in case of plurals form, this is the control for n=1:
    m_textTransSingularForm = nullptr;

    m_pluralNotebook = SegmentedNotebook::Create(this, SegmentStyle::SmallInline);

    sizer->AddSpacer(PX(6));
    sizer->Add(transLineSizer, wxSizerFlags().Expand().Border(wxLEFT, PX(5)));
    sizer->AddSpacer(PX(6));
    sizer->Add(m_textTrans, wxSizerFlags(1).Expand());
    sizer->Add(m_pluralNotebook, wxSizerFlags(1).Expand());

    ShowPluralFormUI(false);

    ColorScheme::SetupWindowColors(this, [=]
    {
        m_fuzzy->SetColors(ColorScheme::Get(Color::FuzzySwitch), ColorScheme::Get(Color::FuzzySwitchInactive));
    #ifdef __WXMSW__
        m_pluralNotebook->SetBackgroundColour(ColorScheme::Get(Color::EditingBackground));
        m_labelTrans->SetBackgroundColour(ColorScheme::Get(Color::EditingThickSeparator));
        m_fuzzy->SetBackgroundColour(ColorScheme::Get(Color::EditingThickSeparator));
    #endif
    });

    m_textTrans->Bind(wxEVT_TEXT, [=](wxCommandEvent& e){ e.Skip(); UpdateFromTextCtrl(); });

    m_fuzzy->Bind(wxEVT_TOGGLEBUTTON, [=](wxCommandEvent& e){
        // The user explicitly changed fuzzy status (e.g. to on). Normally, if the
        // user edits an entry, it's fuzzy flag is cleared, but if the user sets
        // fuzzy on to indicate the translation is problematic and then continues
        // editing the entry, we do not want to annoy him by changing fuzzy back on
        // every keystroke.
        DontAutoclearFuzzyStatus();
        UpdateFromTextCtrl();
        e.Skip();
    });

    m_pluralNotebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [=](wxBookCtrlEvent& e){
        e.Skip();
        UpdateCharCounter(m_associatedList->GetCurrentCatalogItem());
    });
}


void EditingArea::CreateTemplateControls(wxBoxSizer *panelSizer)
{
    auto win = new wxPanel(this, wxID_ANY);
    auto sizer = new wxBoxSizer(wxHORIZONTAL);

    auto explain = new wxStaticText(win, wxID_ANY, _(L"POT files are only templates and don’t contain any translations themselves.\nTo make a translation, create a new PO file based on the template."));
#ifdef __WXOSX__
    explain->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
#endif

    auto button = new ActionButton(
                       win, XRCID("button_new_from_this_pot"), "CreateTranslation",
                       _("Create new translation"),
                       _("Make a new translation from this POT file."));

    sizer->Add(button, wxSizerFlags().CenterVertical().Border(wxLEFT, PX(30)));
    sizer->Add(explain, wxSizerFlags(1).CenterVertical().Border(wxLEFT|wxRIGHT, PX(20)));

    win->SetSizerAndFit(sizer);

    panelSizer->Add(win, 1, wxEXPAND);

    ColorScheme::SetupWindowColors(win, [=]
    {
        explain->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
    });

    win->Bind(wxEVT_PAINT, [win](wxPaintEvent&)
    {
        wxPaintDC dc(win);
        auto clr = ColorScheme::Get(Color::EditingSeparator);
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(0, 1, win->GetClientSize().x, PX(1));
    });
}


void EditingArea::SetupTextCtrlSizes()
{
    int minh = m_textOrig->GetCharHeight();
#ifdef __WXOSX__
    minh += 2*3; // inset
#endif

    m_textOrig->SetMinSize(wxSize(-1, minh));
    m_textOrigPlural->SetMinSize(wxSize(-1, minh));
}


EditingArea::~EditingArea()
{
    // OnPaint may still be called as child windows are destroyed
    m_labelSource = m_labelTrans = nullptr;
}


void EditingArea::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    auto width = dc.GetSize().x;
#ifdef __WXOSX__
    width += 1; // correct for half-pixel undrawn part on the right side
#endif

    const int paddingTop = MACOS_OR_OTHER(dc.GetContentScaleFactor() > 1.0 ? PX(5) : PX(6), PX(6));
    const int paddingBottom = PX(5);

    auto bg = ColorScheme::Get(Color::EditingThickSeparator);
    dc.SetPen(bg);
    dc.SetBrush(bg);
    if (m_labelSource)
    {
        dc.DrawRectangle(0, 0, width, m_labelSource->GetPosition().y + m_labelSource->GetSize().y + paddingBottom);
    }
    if (m_labelTrans)
    {
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y - paddingTop, width, paddingTop + m_labelTrans->GetSize().y + paddingBottom);
    }

    if (m_labelTrans)
    {
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y - paddingTop, width, PX(1));
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y + m_labelTrans->GetSize().y + paddingBottom, width, PX(1));
    }

    auto clr = ColorScheme::Get(Color::EditingSeparator);
    dc.SetPen(clr);
    dc.SetBrush(clr);
    if (m_labelSource)
    {
        dc.DrawRectangle(0, m_labelSource->GetPosition().y + m_labelSource->GetSize().y + paddingBottom, width, PX(1));
    }

    if (m_labelTrans)
    {
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y - paddingTop, width, PX(1));
        dc.DrawRectangle(0, m_labelTrans->GetPosition().y + m_labelTrans->GetSize().y + paddingBottom, width, PX(1));
    }
}



void EditingArea::SetCustomFont(const wxFont& font)
{
    SetCtrlFont(m_textOrig, font);
    SetCtrlFont(m_textOrigPlural, font);
    SetCtrlFont(m_textTrans, font);
    for (auto tp : m_textTransPlural)
        SetCtrlFont(tp, font);

    SetupTextCtrlSizes();
}


bool EditingArea::InitSpellchecker(bool enabled, Language lang)
{
    bool rv = true;

    if (m_textTrans)
    {
        if (!InitTextCtrlSpellchecker(m_textTrans, enabled, lang))
            rv = false;
    }

    for (auto tp : m_textTransPlural)
    {
        if (tp && !InitTextCtrlSpellchecker(tp, enabled, lang))
            rv = false;
    }

    return rv;
}


void EditingArea::SetLanguage(Language lang)
{
    if (m_textTrans)
        m_textTrans->SetLanguage(lang);

    for (auto tp : m_textTransPlural)
    {
        if (tp)
            tp->SetLanguage(lang);
    }
}


void EditingArea::UpdateEditingUIForCatalog(CatalogPtr catalog)
{
    // TODO: ideally we'd do all this at creation time
    if (catalog->UsesSymbolicIDsForSource())
        m_labelSource->SetLabel(_("Source text ID"));
    else
        m_labelSource->SetLabel(_("Source text"));

    m_fuzzyToggleNeeded = m_fuzzy && catalog->HasCapability(Catalog::Cap::FuzzyTranslations);
    if (m_fuzzy)
        m_fuzzy->Show(m_fuzzyToggleNeeded);

    RecreatePluralTextCtrls(catalog);
}

void EditingArea::RecreatePluralTextCtrls(CatalogPtr catalog)
{
    if (!m_pluralNotebook)
        return;

    m_textTransPlural.clear();
    m_pluralNotebook->DeleteAllPages();
    m_textTransSingularForm = NULL;

    auto plurals = PluralFormsExpr(catalog->Header().GetHeader("Plural-Forms").utf8_string());

    int formsCount = catalog->GetPluralFormsCount();
    for (int form = 0; form < formsCount; form++)
    {
        // find example number that would use this plural form:
        static const int maxExamplesCnt = 5;
        wxString examples;
        int firstExample = -1;
        int examplesCnt = 0;

        if (plurals && formsCount > 1)
        {
            for (int example = 0; example < PluralFormsExpr::MAX_EXAMPLES_COUNT; example++)
            {
                if (plurals.evaluate_for_n(example) == form)
                {
                    if (++examplesCnt == 1)
                        firstExample = example;
                    if (examplesCnt == maxExamplesCnt)
                    {
                        examples += L'…';
                        break;
                    }
                    else if (examplesCnt == 1)
                        examples += wxString::Format("%d", example);
                    else
                        examples += wxString::Format(", %d", example);
                }
            }
        }

        wxString desc;
        if (formsCount == 1)
        {
            desc = _("Everything");
        }
        else if (examplesCnt == 0)
        {
            #if 0 // kept just in case, for translations
            desc.Printf(_("Form %i"), form);
            #endif
            desc.Printf(_("Form %i (unused)"), form);
        }
        else if (examplesCnt == 1)
        {
            if (formsCount == 2 && firstExample == 1) // English-like
            {
                desc = _("Singular");
            }
            else
            {
                if (firstExample == 0)
                    desc = _("Zero");
                else if (firstExample == 1)
                    desc = _("One");
                else if (firstExample == 2)
                    desc = _("Two");
                else
                    desc.Printf(L"n = %s", examples);
            }
        }
        else if (formsCount == 2 && examplesCnt == 2 && firstExample == 0 && examples == "0, 1")
        {
            desc = _("Singular");
        }
        else if (formsCount == 2 && firstExample != 1 && examplesCnt == maxExamplesCnt)
        {
            if (firstExample == 0 || firstExample == 2)
                desc = _("Plural");
            else
                desc = _("Other");
        }
        else
            desc.Printf(L"n → %s", examples);

        // create text control and notebook page for it:
        auto txt = new TranslationTextCtrl(m_pluralNotebook, wxID_ANY);
#ifndef __WXOSX__
        txt->SetFont(m_textTrans->GetFont());
#endif
        txt->Bind(wxEVT_TEXT, [=](wxCommandEvent& e){ e.Skip(); UpdateFromTextCtrl(); });
        m_textTransPlural.push_back(txt);
        m_pluralNotebook->AddPage(txt, desc);

        if (examplesCnt == 1 && firstExample == 1) // == singular
            m_textTransSingularForm = txt;
    }

    // as a fallback, assume 1st form for plural entries is the singular
    // (like in English and most real-life uses):
    if (!m_textTransSingularForm && !m_textTransPlural.empty())
        m_textTransSingularForm = m_textTransPlural[0];
}


void EditingArea::ShowPluralFormUI(bool show)
{
    wxSizer *origSizer = m_textOrig->GetContainingSizer();
    origSizer->Show(m_labelSingular, show);
    origSizer->Show(m_labelPlural, show);
    origSizer->Show(m_textOrigPlural, show);
    origSizer->Layout();

    if (m_textTrans && m_pluralNotebook)
    {
        wxSizer *textSizer = m_textTrans->GetContainingSizer();
        textSizer->Show(m_textTrans, !show);
        textSizer->Show(m_pluralNotebook, show);
        textSizer->Layout();
    }
}


void EditingArea::ShowPart(wxWindow *part, bool show)
{
    if (part)
        part->GetContainingSizer()->Show(part, show);
}


void EditingArea::SetSingleSelectionMode()
{
    if (m_isSingleSelection)
        return;
    m_isSingleSelection = true;

    if (m_fuzzy)
        m_fuzzy->Show(m_fuzzyToggleNeeded);
    m_charCounter->Show();

    Enable();
}


void EditingArea::SetMultipleSelectionMode()
{
    if (!m_isSingleSelection)
        return;
    m_isSingleSelection = false;

    // TODO: Show better UI

    if (m_fuzzy)
        m_fuzzy->Hide();
    m_charCounter->Hide();
    ShowPluralFormUI(false);
    ShowPart(m_tagIdOrContext, false);
    ShowPart(m_tagFormat, false);
    ShowPart(m_tagPretranslated, false);
    ShowPart(m_issueLine, false);

    m_textOrig->Clear();
    if (m_textTrans)
        m_textTrans->Clear();
    Disable();
}


void EditingArea::SetTextFocus()
{
    if (m_textTrans && m_textTrans->IsShown())
        m_textTrans->SetFocus();
    else if (!m_textTransPlural.empty())
    {
        if (m_pluralNotebook && m_pluralNotebook->GetPageCount())
            m_pluralNotebook->SetSelection(0);
        m_textTransPlural[0]->SetFocus();
    }
}

bool EditingArea::HasTextFocus()
{
    wxWindow *focus = wxWindow::FindFocus();
    return (focus == m_textTrans) ||
           (focus && focus->GetParent() == m_pluralNotebook);
}


bool EditingArea::HasTextFocusInPlurals()
{
    if (!m_pluralNotebook || !m_pluralNotebook->IsShown())
        return false;

    auto focused = dynamic_cast<TranslationTextCtrl*>(FindFocus());
    if (!focused)
        return false;

    return std::find(m_textTransPlural.begin(), m_textTransPlural.end(), focused) != m_textTransPlural.end();
}


bool EditingArea::IsShowingPlurals()
{
    return m_pluralNotebook && m_pluralNotebook->IsShown();
}


void EditingArea::CopyFromSingular()
{
    auto current = dynamic_cast<TranslationTextCtrl*>(wxWindow::FindFocus());
    if (!current || !m_textTransSingularForm)
        return;

    current->SetPlainTextUserWritten(m_textTransSingularForm->GetPlainText());
}


void EditingArea::UpdateToTextCtrl(CatalogItemPtr item, int flags)
{
    if (!(flags & DontTouchText))
    {
        auto syntax = SyntaxHighlighter::ForItem(*item);
        m_textOrig->SetSyntaxHighlighter(syntax);
        if (m_textTrans)
            m_textTrans->SetSyntaxHighlighter(syntax);
        if (item->HasPlural())
        {
            m_textOrigPlural->SetSyntaxHighlighter(syntax);
            for (auto p : m_textTransPlural)
                p->SetSyntaxHighlighter(syntax);
        }

        m_textOrig->SetPlainText(item->GetString());

        if (item->HasPlural())
        {
            m_textOrigPlural->SetPlainText(item->GetPluralString());

            unsigned formsCnt = (unsigned)m_textTransPlural.size();
            for (unsigned j = 0; j < formsCnt; j++)
                SetTranslationValue(m_textTransPlural[j], wxEmptyString, flags);

            unsigned i = 0;
            for (i = 0; i < std::min(formsCnt, item->GetNumberOfTranslations()); i++)
            {
                SetTranslationValue(m_textTransPlural[i], item->GetTranslation(i), flags);
            }

            if ((flags & EditingArea::ItemChanged) && m_pluralNotebook && m_pluralNotebook->GetPageCount())
                m_pluralNotebook->SetSelection(0);
        }
        else
        {
            if (m_textTrans)
                SetTranslationValue(m_textTrans, item->GetTranslation(), flags);
        }
    } // !DontTouchText

    ShowPart(m_tagIdOrContext, item->HasContext() || item->HasSymbolicId());
    if (item->HasContext())
    {
        m_tagIdOrContext->SetColor(Color::TagContextFg, Color::TagContextBg);
        m_tagIdOrContext->SetLabel(item->GetContext());
        // TRANSLATORS: Tooltip on message context tag in the editing area, '%s' is the context text
        m_tagIdOrContext->SetToolTip(wxString::Format(_("String context: %s"), item->GetContext()));
    }
    else if (item->HasSymbolicId())
    {
        m_tagIdOrContext->SetColor(Color::TagSecondaryFg, Color::TagSecondaryBg);
        m_tagIdOrContext->SetLabel(item->GetSymbolicId());
        // TRANSLATORS: Tooltip on string ID tag in the editing area, '%s' contains the ID
        m_tagIdOrContext->SetToolTip(wxString::Format(_("String identifier: %s"), item->GetSymbolicId()));
    }

    auto format = item->GetFormatFlag();
    ShowPart(m_tagFormat, !format.empty());
    if (!format.empty())
    {
        // TRANSLATORS: %s is replaced with language name, e.g. "PHP" or "C", so "PHP Format" etc."
        m_tagFormat->SetLabel(wxString::Format(MSW_OR_OTHER(_("%s format"), _("%s Format")), PrettyPrintFormatTag(format)));
    }

    if (m_fuzzy)
        m_fuzzy->SetValue(item->IsFuzzy());

    UpdateAuxiliaryInfo(item);

    ShowPluralFormUI(item->HasPlural());

    Layout();

    Refresh();

    // by default, editing fuzzy item unfuzzies it
    m_dontAutoclearFuzzyStatus = false;
}


void EditingArea::UpdateAuxiliaryInfo(CatalogItemPtr item)
{
    if (m_tagPretranslated)
        ShowPart(m_tagPretranslated, item->IsPreTranslated());

    if (m_issueLine)
    {
        if (item->HasIssue())
        {
            m_issueLine->SetIssue(item->GetIssue());
            ShowPart(m_issueLine, true);
        }
        else
        {
            ShowPart(m_issueLine, false);
        }
        Layout();
    }

    UpdateCharCounter(item);
}

void EditingArea::UpdateCharCounter(CatalogItemPtr item)
{
    if (!m_charCounter || !item)
        return;

    if (item->HasPlural() && m_pluralNotebook)
    {
        int index = m_pluralNotebook->GetSelection();
        if (index == 0)
            m_charCounter->UpdateSourceLength((int)item->GetString().length());
        else
            m_charCounter->UpdateSourceLength((int)item->GetPluralString().length());
        m_charCounter->UpdateTranslationLength((int)item->GetTranslation(index).length());
    }
    else
    {
        m_charCounter->UpdateSourceLength((int)item->GetString().length());
        m_charCounter->UpdateTranslationLength((int)item->GetTranslation().length());
    }
}


void EditingArea::UpdateFromTextCtrl()
{
    if (!m_isSingleSelection)
        return;

    auto item = m_associatedList->GetCurrentCatalogItem();
    if (!item)
        return;

    wxString key = item->GetString();
    bool newfuzzy = m_fuzzy->GetValue();

    const bool oldIsTranslated = item->IsTranslated();
    bool allTranslated = true; // will be updated later
    bool anyTransChanged = false; // ditto

    if (item->HasPlural())
    {
        wxArrayString str;
        for (unsigned i = 0; i < m_textTransPlural.size(); i++)
        {
            auto val = PreprocessEnteredTextForItem(item, m_textTransPlural[i]->GetPlainText());
            str.Add(val);
            if ( val.empty() )
                allTranslated = false;
        }

        if ( str != item->GetTranslations() )
        {
            anyTransChanged = true;
            item->SetTranslations(str);
        }
    }
    else
    {
        auto newval = PreprocessEnteredTextForItem(item, m_textTrans->GetPlainText());

        if ( newval.empty() )
            allTranslated = false;

        if ( newval != item->GetTranslation() )
        {
            anyTransChanged = true;
            item->SetTranslation(newval);
        }
    }

    if (item->IsFuzzy() == newfuzzy && !anyTransChanged)
    {
        return; // not even fuzzy status changed, so return
    }

    // did something affecting statistics change?
    bool statisticsChanged = false;

    if (newfuzzy == item->IsFuzzy() && !m_dontAutoclearFuzzyStatus)
        newfuzzy = false;

    if ( item->IsFuzzy() != newfuzzy )
    {
        item->SetFuzzy(newfuzzy);
        m_fuzzy->SetValue(newfuzzy);
        statisticsChanged = true;
    }
    if ( oldIsTranslated != allTranslated )
    {
        item->SetTranslated(allTranslated);
        statisticsChanged = true;
    }
    item->SetModified(true);
    item->SetPreTranslated(false);

    UpdateAuxiliaryInfo(item);

    m_associatedList->RefreshItem(m_associatedList->GetCurrentItem());

    if (OnUpdatedFromTextCtrl)
        OnUpdatedFromTextCtrl(item, statisticsChanged);
}


void EditingArea::ChangeFocusedPluralTab(int offset)
{
    wxCHECK_RET(offset == +1 || offset == -1, "invalid offset");

    bool hasFocus = HasTextFocusInPlurals();
#ifdef __WXMSW__
    wxWindow *prevFocus = hasFocus ? nullptr : FindFocus();
#endif

    m_pluralNotebook->AdvanceSelection(/*forward=*/offset == +1 ? true : false);
    if (hasFocus)
        m_textTransPlural[m_pluralNotebook->GetSelection()]->SetFocus();
#ifdef __WXMSW__
    else if (prevFocus)
        prevFocus->SetFocus();
#endif
}


int EditingArea::GetTopRowHeight() const
{
    return m_tagIdOrContext->GetContainingSizer()->GetSize().y;
}
