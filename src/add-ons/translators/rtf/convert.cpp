/*
 * Copyright 2004-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include "convert.h"

#include <algorithm>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <ByteOrder.h>
#include <File.h>
#include <Font.h>
#include <fs_attr.h>
#include <TextView.h>
#include <TranslatorFormats.h>
#include <TypeConstants.h>

#include <AutoDeleter.h>

#include "Stack.h"


#define READ_BUFFER_SIZE 2048


struct conversion_context {
	conversion_context()
	{
		Reset();
	}

	void Reset();

	int32	section;
	int32	page;
	int32	start_page;
	int32	first_line_indent;
	bool	new_line;
};


class TextOutput : public RTF::Worker {
	public:
		TextOutput(RTF::Header &start, BDataIO *stream, bool processRuns);
		~TextOutput();

		size_t Length() const;
		void *FlattenedRunArray(int32 &size);

	protected:
		virtual void Group(RTF::Group *group);
		virtual void GroupEnd(RTF::Group *group);
		virtual void Command(RTF::Command *command);
		virtual void Text(RTF::Text *text);

	private:
		void PrepareTextRun(text_run *current);

		BDataIO				*fTarget;
		int32				fOffset;
		conversion_context	fContext;
		Stack<text_run *>	fGroupStack;
		bool				fProcessRuns;
		BList				fRuns;
		text_run			*fCurrentRun;
		BApplication		*fApplication;
};


void
conversion_context::Reset()
{
	section = 1;
	page = 1;
	start_page = page;
	first_line_indent = 0;
	new_line = true;
}


//	#pragma mark -


static size_t
write_text(conversion_context &context, const char *text, size_t length,
	BDataIO *target = NULL)
{
	size_t prefix = 0;
	if (context.new_line) {
		prefix = context.first_line_indent;
		context.new_line = false;
	}

	if (target == NULL)
		return prefix + length;

	for (uint32 i = 0; i < prefix; i++) {
		write_text(context, " ", 1, target);
	}

	ssize_t written = target->Write(text, length);
	if (written < B_OK)
		throw (status_t)written;
	else if ((size_t)written != length)
		throw (status_t)B_IO_ERROR;

	return prefix + length;
}


static size_t
write_text(conversion_context &context, const char *text,
	BDataIO *target = NULL)
{
	return write_text(context, text, strlen(text), target);
}


static size_t
next_line(conversion_context &context, const char *prefix,
	BDataIO *target)
{
	size_t length = strlen(prefix);
	context.new_line = true;

	if (target != NULL) {
		ssize_t written = target->Write(prefix, length);
		if (written < B_OK)
			throw (status_t)written;
		else if ((size_t)written != length)
			throw (status_t)B_IO_ERROR;
	}

	return length;
}


static size_t
write_unicode_char(conversion_context &context, uint32 c,
	BDataIO *target)
{
	size_t length = 1;
	char bytes[4];

	if (c < 0x80)
		bytes[0] = c;
	else if (c < 0x800) {
		bytes[0] = 0xc0 | (c >> 6);
		bytes[1] = 0x80 | (c & 0x3f);
		length = 2;
	} else if (c < 0x10000) {
		bytes[0] = 0xe0 | (c >> 12);
		bytes[1] = 0x80 | ((c >> 6) & 0x3f);
		bytes[2] = 0x80 | (c & 0x3f);
		length = 3;
	} else if (c <= 0x10ffff) {
		bytes[0] = 0xf0 | (c >> 18);
		bytes[1] = 0x80 | ((c >> 12) & 0x3f);
		bytes[2] = 0x80 | ((c >> 6) & 0x3f);
		bytes[3] = 0x80 | (c & 0x3f);
		length = 4;
	}

	return write_text(context, bytes, length, target);
}


static size_t
process_command(conversion_context &context, RTF::Command *command,
	BDataIO *target)
{
	const char *name = command->Name();

	if (!strcmp(name, "par") || !strcmp(name, "line")) {
		// paragraph ended
		return next_line(context, "\n", target);
	}
	if (!strcmp(name, "sect")) {
		// section ended
		context.section++;
		return next_line(context, "\n", target);
	}
	if (!strcmp(name, "page")) {
		// we just insert two carriage returns for a page break
		context.page++;
		return next_line(context, "\n\n", target);
	}
	if (!strcmp(name, "tab")) {
		return write_text(context, "\t", target);
	}
	if (!strcmp(name, "'")) {
		return write_unicode_char(context, command->Option(), target);
	}

	if (!strcmp(name, "pard")) {
		// reset paragraph
		context.first_line_indent = 0;
		return 0;
	}
	if (!strcmp(name, "fi") || !strcmp(name, "cufi")) {
		// "cufi" first line indent in 1/100 space steps
		// "fi" is most probably specified in 1/20 pts
		// Currently, we don't differentiate between the two...
		context.first_line_indent = (command->Option() + 50) / 100;
		if (context.first_line_indent < 0)
			context.first_line_indent = 0;
		if (context.first_line_indent > 8)
			context.first_line_indent = 8;

		return 0;
	}

	// document variables

	if (!strcmp(name, "sectnum")) {
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%" B_PRId32, context.section);
		return write_text(context, buffer, target);
	}
	if (!strcmp(name, "pgnstarts")) {
		context.start_page = command->HasOption() ? command->Option() : 1;
		return 0;
	}
	if (!strcmp(name, "pgnrestart")) {
		context.page = context.start_page;
		return 0;
	}
	if (!strcmp(name, "chpgn")) {
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%" B_PRId32, context.page);
		return write_text(context, buffer, target);
	}
	return 0;
}


static void
set_font_face(BFont &font, uint16 face, bool on)
{
	// Special handling for B_REGULAR_FACE, since BFont::SetFace(0)
	// just doesn't do anything

	if (font.Face() == B_REGULAR_FACE && on)
		font.SetFace(face);
	else if ((font.Face() & ~face) == 0 && !on)
		font.SetFace(B_REGULAR_FACE);
	else if (on)
		font.SetFace(font.Face() | face);
	else
		font.SetFace(font.Face() & ~face);
}


static bool
text_runs_are_equal(text_run *a, text_run *b)
{
	if (a == NULL && b == NULL)
		return true;

	if (a == NULL || b == NULL)
		return false;

	return a->offset == b->offset
		&& *(uint32*)&a->color == *(uint32*)&b->color
		&& a->font == b->font;
}


static text_run *
copy_text_run(text_run *run)
{
	static const rgb_color kBlack = {0, 0, 0, 255};

	text_run *newRun = new text_run();
	if (newRun == NULL)
		throw (status_t)B_NO_MEMORY;

	if (run != NULL) {
		newRun->offset = run->offset;
		newRun->font = run->font;
		newRun->color = run->color;
	} else {
		newRun->offset = 0;
		newRun->color = kBlack;
	}

	return newRun;
}


#if 0
void
dump_text_run(text_run *run)
{
	if (run == NULL)
		return;

	printf("run: offset = %ld, color = {%d,%d,%d}, font = ",
		run->offset, run->color.red, run->color.green, run->color.blue);
	run->font.PrintToStream();
}
#endif


//	#pragma mark -


TextOutput::TextOutput(RTF::Header &start, BDataIO *stream, bool processRuns)
	: RTF::Worker(start),
	fTarget(stream),
	fOffset(0),
	fProcessRuns(processRuns),
	fCurrentRun(NULL),
	fApplication(NULL)
{
	// This is not nice, but it's the only we can provide all features on command
	// line tools that don't create a BApplication - without a BApplication, we
	// could not support any text styles (colors and fonts)

	if (processRuns && be_app == NULL)
		fApplication = new BApplication("application/x-vnd.Haiku-RTFTranslator");
}


TextOutput::~TextOutput()
{
	delete fApplication;
}


size_t
TextOutput::Length() const
{
	return (size_t)fOffset;
}


void *
TextOutput::FlattenedRunArray(int32 &_size)
{
	// are there any styles?
	if (fRuns.CountItems() == 0) {
		_size = 0;
		return NULL;
	}

	// create array

	text_run_array *array = (text_run_array *)malloc(sizeof(text_run_array)
		+ sizeof(text_run) * (fRuns.CountItems() - 1));
	if (array == NULL)
		throw (status_t)B_NO_MEMORY;

	array->count = fRuns.CountItems();

	for (int32 i = 0; i < array->count; i++) {
		text_run *run = (text_run *)fRuns.RemoveItem((int32)0);
		array->runs[i] = *run;
		delete run;
	}

	void *flattenedRunArray = BTextView::FlattenRunArray(array, &_size);

	free(array);

	return flattenedRunArray;
}


void
TextOutput::PrepareTextRun(text_run *run)
{
	if (run != NULL && fOffset == run->offset)
		return;

	text_run *newRun = copy_text_run(run);

	newRun->offset = fOffset;

	fRuns.AddItem(newRun);
	fCurrentRun = newRun;
}


void
TextOutput::Group(RTF::Group *group)
{
	if (group->Destination() != RTF::TEXT_DESTINATION) {
		Skip();
		return;
	}

	if (!fProcessRuns)
		return;

	// We only push a copy of the run on the stack because the current
	// run may still be changed in the new group -- later, we'll just
	// see if that was the case, and either use the copied one then,
	// or throw it away
	text_run *run = NULL;
	if (fCurrentRun != NULL)
		run = copy_text_run(fCurrentRun);

	fGroupStack.Push(run);
}


void
TextOutput::GroupEnd(RTF::Group *group)
{
	if (!fProcessRuns)
		return;

	text_run *last = NULL;
	fGroupStack.Pop(&last);

	// has the style been changed?
	if (!text_runs_are_equal(last, fCurrentRun)) {
		if (fCurrentRun != NULL && last != NULL
			&& fCurrentRun->offset == fOffset) {
			// replace the current one, we don't need it anymore
			fCurrentRun->color = last->color;
			fCurrentRun->font = last->font;
			delete last;
		} else if (last) {
			// adopt the text_run from the previous group
			last->offset = fOffset;
			fRuns.AddItem(last);
			fCurrentRun = last;
		}
	} else
		delete last;
}


void
TextOutput::Command(RTF::Command *command)
{
	if (!fProcessRuns) {
		fOffset += process_command(fContext, command, fTarget);
		return;
	}

	const char *name = command->Name();

	if (!strcmp(name, "cf")) {
		// foreground color
		PrepareTextRun(fCurrentRun);
		fCurrentRun->color = Start().Color(command->Option());
	} else if (!strcmp(name, "b")
		|| !strcmp(name, "embo") || !strcmp(name, "impr")) {
		// bold style ("emboss" and "engrave" are currently the same, too)
		PrepareTextRun(fCurrentRun);
		set_font_face(fCurrentRun->font, B_BOLD_FACE, command->Option() != 0);
	} else if (!strcmp(name, "i")) {
		// italic style
		PrepareTextRun(fCurrentRun);
		set_font_face(fCurrentRun->font, B_ITALIC_FACE, command->Option() != 0);
	} else if (!strcmp(name, "ul")) {
		// underscore style
		PrepareTextRun(fCurrentRun);
		set_font_face(fCurrentRun->font, B_UNDERSCORE_FACE, command->Option() != 0);
	} else if (!strcmp(name, "strike")) {
		// strikeout style
		PrepareTextRun(fCurrentRun);
		set_font_face(fCurrentRun->font, B_STRIKEOUT_FACE, command->Option() != 0);
	} else if (!strcmp(name, "fs")) {
		// font size in half points
		PrepareTextRun(fCurrentRun);
		fCurrentRun->font.SetSize(command->Option() / 2.0);
	} else if (!strcmp(name, "plain")) {
		// reset font to plain style
		PrepareTextRun(fCurrentRun);
		fCurrentRun->font = be_plain_font;
	} else if (!strcmp(name, "f")) {
		// font number
		RTF::Group *fonts = Start().FindGroup("fonttbl");
		if (fonts == NULL)
			return;

		PrepareTextRun(fCurrentRun);
		BFont font;
			// missing font info will be replaced by the default font

		RTF::Command *info;
		for (int32 index = 0; (info = fonts->FindDefinition("f", index))
			!= NULL; index++) {
			if (info->Option() != command->Option())
				continue;

			// ToDo: really try to choose font by name and serif/sans-serif
			// ToDo: the font list should be built before once

			// For now, it only differentiates fixed fonts from proportional ones
			if (fonts->FindDefinition("fmodern", index) != NULL)
				font = be_fixed_font;
		}

		font_family family;
		font_style style;
		font.GetFamilyAndStyle(&family, &style);

		fCurrentRun->font.SetFamilyAndFace(family, fCurrentRun->font.Face());
	} else
		fOffset += process_command(fContext, command, fTarget);
}


void
TextOutput::Text(RTF::Text *text)
{
	fOffset += write_text(fContext, text->String(), text->Length(), fTarget);
}


//	#pragma mark -


status_t
convert_to_stxt(RTF::Header &header, BDataIO &target)
{
	// count text bytes

	size_t textSize = 0;

	try {
		TextOutput counter(header, NULL, false);

		counter.Work();
		textSize = counter.Length();
	} catch (status_t status) {
		return status;
	}

	// put out header

	TranslatorStyledTextStreamHeader stxtHeader;
	stxtHeader.header.magic = 'STXT';
	stxtHeader.header.header_size = sizeof(TranslatorStyledTextStreamHeader);
	stxtHeader.header.data_size = 0;
	stxtHeader.version = 100;
	status_t status = swap_data(B_UINT32_TYPE, &stxtHeader, sizeof(stxtHeader),
		B_SWAP_HOST_TO_BENDIAN);
	if (status != B_OK)
		return status;

	ssize_t written = target.Write(&stxtHeader, sizeof(stxtHeader));
	if (written < B_OK)
		return written;
	if (written != sizeof(stxtHeader))
		return B_IO_ERROR;

	TranslatorStyledTextTextHeader textHeader;
	textHeader.header.magic = 'TEXT';
	textHeader.header.header_size = sizeof(TranslatorStyledTextTextHeader);
	textHeader.header.data_size = textSize;
	textHeader.charset = B_UNICODE_UTF8;
	status = swap_data(B_UINT32_TYPE, &textHeader, sizeof(textHeader),
		B_SWAP_HOST_TO_BENDIAN);
	if (status != B_OK)
		return status;

	written = target.Write(&textHeader, sizeof(textHeader));
	if (written < B_OK)
		return written;
	if (written != sizeof(textHeader))
		return B_IO_ERROR;

	// put out main text

	void *flattenedRuns = NULL;
	int32 flattenedSize = 0;

	try {
		TextOutput output(header, &target, true);

		output.Work();
		flattenedRuns = output.FlattenedRunArray(flattenedSize);
	} catch (status_t status) {
		return status;
	}

	BPrivate::MemoryDeleter _(flattenedRuns);

	// put out styles

	TranslatorStyledTextStyleHeader styleHeader;
	styleHeader.header.magic = 'STYL';
	styleHeader.header.header_size = sizeof(TranslatorStyledTextStyleHeader);
	styleHeader.header.data_size = flattenedSize;
	styleHeader.apply_offset = 0;
	styleHeader.apply_length = textSize;

	status = swap_data(B_UINT32_TYPE, &styleHeader, sizeof(styleHeader),
		B_SWAP_HOST_TO_BENDIAN);
	if (status != B_OK)
		return status;

	written = target.Write(&styleHeader, sizeof(styleHeader));
	if (written < B_OK)
		return written;
	if (written != sizeof(styleHeader))
		return B_IO_ERROR;

	// output actual style information
	written = target.Write(flattenedRuns, flattenedSize);

	if (written < B_OK)
		return written;
	if (written != flattenedSize)
		return B_IO_ERROR;

	return B_OK;
}


status_t
convert_to_plain_text(RTF::Header &header, BPositionIO &target)
{
	// put out main text

	void *flattenedRuns = NULL;
	int32 flattenedSize = 0;

	// TODO: this is not really nice, we should adopt the BPositionIO class
	//	from Dano/Zeta which has meta data support
	BFile *file = dynamic_cast<BFile *>(&target);

	try {
		TextOutput output(header, &target, file != NULL);

		output.Work();
		flattenedRuns = output.FlattenedRunArray(flattenedSize);
	} catch (status_t status) {
		return status;
	}

	if (file == NULL) {
		// we can't write the styles
		return B_OK;
	}

	// put out styles

	ssize_t written = file->WriteAttr("styles", B_RAW_TYPE, 0, flattenedRuns,
		flattenedSize);
	if (written >= B_OK && written != flattenedSize)
		file->RemoveAttr("styles");

	free(flattenedRuns);
	return B_OK;
}

struct color_compare
{
	bool operator()(const rgb_color& left, const rgb_color& right) const
	{
		return (*(const uint32 *)&left) < (*(const uint32 *)&right);
	}
};

status_t convert_styled_text_to_rtf(
	BPositionIO* source, BPositionIO* target)
{
	if (source->Seek(0, SEEK_SET) != 0)
		return B_ERROR;

	const ssize_t kstxtsize = sizeof(TranslatorStyledTextStreamHeader);
	const ssize_t ktxtsize = sizeof(TranslatorStyledTextTextHeader);
	TranslatorStyledTextStreamHeader stxtheader;
	TranslatorStyledTextTextHeader txtheader;
	char buffer[READ_BUFFER_SIZE];
	
	// Read STXT and TEXT headers
	if (source->Read(&stxtheader, kstxtsize) != kstxtsize)
		return B_ERROR;
	if (source->Read(&txtheader, ktxtsize) != ktxtsize
		|| swap_data(B_UINT32_TYPE, &txtheader,
			sizeof(TranslatorStyledTextTextHeader),
			B_SWAP_BENDIAN_TO_HOST) != B_OK)
		return B_ERROR;
	
	// source now points to the beginning of the plain text section
	BString plainText;
	ssize_t nread = 0, nreed = 0, ntotalread = 0;
	nreed = std::min((size_t)READ_BUFFER_SIZE,
		(size_t)txtheader.header.data_size - ntotalread);
	nread = source->Read(buffer, nreed);
	while (nread > 0) {
		plainText << buffer;

		ntotalread += nread;
		nreed = std::min((size_t)READ_BUFFER_SIZE,
			(size_t)txtheader.header.data_size - ntotalread);
		nread = source->Read(buffer, nreed);
	}

	if ((ssize_t)txtheader.header.data_size != ntotalread)
		return B_NO_TRANSLATOR;
		
	BString rtfFile =
		"{\\rtf1\\ansi";

	ssize_t read = 0;
	TranslatorStyledTextStyleHeader stylHeader;
	read = source->Read(buffer, sizeof(stylHeader));

	if (read < 0)
		return B_ERROR;

	if (read != sizeof(stylHeader) && read != 0)
		return B_NO_TRANSLATOR;

	if (read == sizeof(stylHeader)) { // There is a STYL section
		memcpy(&stylHeader, buffer, sizeof(stylHeader));
		if (swap_data(B_UINT32_TYPE, &stylHeader, sizeof(stylHeader),
			B_SWAP_BENDIAN_TO_HOST) != B_OK) {
			return B_ERROR;
		}

		if (stylHeader.header.magic != 'STYL'
			|| stylHeader.header.header_size != sizeof(stylHeader)) {
			return B_NO_TRANSLATOR;
		}

		uint8 unflattened[stylHeader.header.data_size];
		source->Read(unflattened, stylHeader.header.data_size);
		text_run_array *styles = BTextView::UnflattenRunArray(unflattened);

		// RTF needs us to mention font and color names in advance so
		// we collect them in sets
		std::set<rgb_color, color_compare> colorTable;
		std::set<BString> fontTable;

		font_family out;
		for (int i = 0; i < styles->count; i++) {
			colorTable.insert(styles->runs[i].color);
			styles->runs[i].font.GetFamilyAndStyle(&out, NULL);
			fontTable.insert(BString(out));
		}

		// Now we write them to the file
		std::set<BString>::iterator it;
		uint32 count = 0;

		rtfFile << "{\\fonttbl";
		for (it = fontTable.begin(); it != fontTable.end(); it++) {
			rtfFile << "{\\f" << count << " " << *it << ";}";
			count++;
		}
		rtfFile << "}{\\colortbl";

		std::set<rgb_color, color_compare>::iterator cit;
		for (cit = colorTable.begin(); cit != colorTable.end(); cit++) {
			rtfFile << "\\red" << cit->red
				<< "\\green" << cit->green
				<< "\\blue" << cit->blue
				<< ";";
		}
		rtfFile << "}";

		// Now we put out the actual text with styling information run by run
		for (int i = 0; i < styles->count; i++) {
			// Find font and color indices
			styles->runs[i].font.GetFamilyAndStyle(&out, NULL);
			int fontIndex = std::distance(fontTable.begin(),
				fontTable.find(BString(out)));
			int colorIndex = std::distance(colorTable.begin(),
				colorTable.find(styles->runs[i].color));
			rtfFile << "\\pard\\plain\\f" << fontIndex << "\\cf" << colorIndex;

			// Apply various font styles
			uint16 fontFace = styles->runs[i].font.Face();
			if (fontFace & B_ITALIC_FACE)
				rtfFile << "\\i";
			if (fontFace & B_UNDERSCORE_FACE)
				rtfFile << "\\ul";
			if (fontFace & B_BOLD_FACE)
				rtfFile << "\\b";
			if (fontFace & B_STRIKEOUT_FACE)
				rtfFile << "\\strike";

			// RTF font size unit is half-points, but BFont::Size() returns
			// points
			rtfFile << "\\fs"
				<< static_cast<int>(styles->runs[i].font.Size() * 2);

			int length;
			if (i < styles->count - 1) {
				length = styles->runs[i + 1].offset - styles->runs[i].offset;
			} else {
				length = plainText.Length() - styles->runs[i].offset;
			}

			BString segment;
			plainText.CopyInto(segment, styles->runs[i].offset, length);

			// Escape control structures
			segment.CharacterEscape("\\{}", '\\');
			segment.ReplaceAll("\n", "\\line");

			rtfFile << " " << segment;
		}

		BTextView::FreeRunArray(styles);

		rtfFile << "}";
	} else {
		// There is no STYL section
		// Just use a generic preamble
		rtfFile << "{\\fonttbl\\f0 Noto Sans;}\\f0\\pard " << plainText
			<< "}";
	}
	
	target->Write(rtfFile.String(), rtfFile.Length());

	return B_OK;
}

		
status_t convert_plain_text_to_rtf(
	BPositionIO& source, BPositionIO& target)
{
	BString rtfFile = 
		"{\\rtf1\\ansi{\\fonttbl\\f0\\fswiss Helvetica;}\\f0\\pard ";
		
	BFile* fileSource = (BFile*)&source;
	off_t size;
	fileSource->GetSize(&size);
	char* sourceBuf = (char*)malloc(size);
	fileSource->Read((void*)sourceBuf, size);
	
	BString sourceTxt = sourceBuf;
	sourceTxt.CharacterEscape("\\{}", '\\');
	sourceTxt.ReplaceAll("\n", " \\par ");
	rtfFile << sourceTxt << " }";
	
	BFile* fileTarget = (BFile*)&target;
	fileTarget->Write((const void*)rtfFile, rtfFile.Length());
	
	return B_OK;
}
