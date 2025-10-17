/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2017-2025 Vaclav Slavik
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

#ifndef Poedit_extractor_h
#define Poedit_extractor_h

#include <map>
#include <memory>
#include <stdexcept>
#include <set>
#include <vector>

#include <wx/string.h>

#include "gexecute.h"
#include "utility.h"


/// Specification of the source code to search.
struct SourceCodeSpec
{
    wxString BasePath;
    wxArrayString SearchPaths;
    wxArrayString ExcludedPaths;

    wxArrayString Keywords;
    wxString Charset;

    std::vector<std::pair<wxString, wxString>> TypeMapping;

    // additional keys from the headers
    std::map<wxString, wxString> XHeaders;
};

enum class ExtractionError
{
    Unspecified,
    NoSourcesFound,
    PermissionDenied
};

class ExtractionException : public std::runtime_error
{
public:
    ExtractionException(ExtractionError error_, const wxString& file_ = wxString())
        : std::runtime_error("extraction error"), error(error_), file(file_) {}

    ExtractionError error;
    wxString file;
};


/// Complete result of running an extraction task.
struct ExtractionOutput
{
    /// POT file containing extracted strings.
    wxString pot_file;

    /// Errors/warnings that occurred during extraction.
    ParsedGettextErrors errors;

    explicit operator bool() const { return !pot_file.empty(); }
};


/**
    Base class for extractors -- implementations of extracting translations
    from source code.
 */
class Extractor
{
public:
    typedef std::vector<std::shared_ptr<Extractor>> ExtractorsList;
    typedef std::vector<wxString> FilesList;

    // Static helper methods:

    /**
        Returns all available extractor implementations.

        Extractors are listed in their priority and should be used in this
        order, i.e. subsequent extractors should only be used to process files
        not yet handled by previous extractors.
     */
    static ExtractorsList CreateAllExtractors(const SourceCodeSpec& sources);

    /**
        Collects all files from source code, possibly including files that
        don't contain translations.

        The returned list is guaranteed to be sorted by operator<

        May throw ExtractionException.
     */
    static FilesList CollectAllFiles(const SourceCodeSpec& sources);


    /**
        Extracts translations from given source files using all
        available extractors.
     */
    static ExtractionOutput ExtractWithAll(TempDirectory& tmpdir,
                                           const SourceCodeSpec& sourceSpec,
                                           const std::vector<wxString>& files);

    // Extractor helpers:

    /// Returns only those files from @a files that are supported by this extractor.
    FilesList FilterFiles(const FilesList& files) const;

    // Extractor API for derived classes:

    /// Returns extractor's symbolic name
    virtual wxString GetId() const = 0;

    /// Priority value for GetPriority()
    enum class Priority
    {
        Highest                     =   1,
        CustomExtension             =   2, // customization should be highest
        High                        =  10,
        DefaultSpecializedExtension =  95, // for use with e.g. .blade.php extentions
        Default                     = 100
    };

    /// Returns priority of the extractor
    Priority GetPriority() const { return m_priority; }

    /// Sets extractor's priority
    void SetPriority(Priority p) { m_priority = p; }

    /**
        Returns whether the file is recognized.
        
        Default implementation uses extension and wildcard matching, see
        RegisterExtension() and RegisterWildcard().
      */
    virtual bool IsFileSupported(const wxString& file) const;

    /// Add a known extension or wildcard to be used by default IsFileSupported
    /// (called from ctors)
    void RegisterExtension(const wxString& ext);
    void RegisterWildcard(const wxString& wildcard);

    /**
        Extracts translations from given source files using all
        available extractors.
     */
    virtual ExtractionOutput Extract(TempDirectory& tmpdir,
                                     const SourceCodeSpec& sourceSpec,
                                     const std::vector<wxString>& files) const = 0;

protected:
    Extractor() : m_priority(Priority::Default) {}
    virtual ~Extractor() {}

    /// Check if file is supported based on its extension
    bool HasKnownExtension(const wxString& file) const;

    /// Concatenates partial outputs using msgcat
    static ExtractionOutput ConcatPartials(TempDirectory& tmpdir, const std::vector<ExtractionOutput>& partials);

private:
    Priority m_priority;
    std::set<wxString> m_extensions;
    std::vector<wxString> m_wildcards;

protected:
    // private factories:
    static void CreateAllLegacyExtractors(ExtractorsList& into, const SourceCodeSpec& sources);
    static void CreateGettextExtractors(ExtractorsList& into, const SourceCodeSpec& sources);
};

#endif // Poedit_extractor_h
