#include <algorithm>
#include <chrono>
#include <string>
#include <regex>
#include <cmath>

#include "TextEditor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h" // for imGui::GetCurrentWindow()

// TODO
// - multiline comments vs single-line: latter is blocking start of a ML

template<class InputIt1, class InputIt2, class BinaryPredicate>
bool equals(InputIt1 first1, InputIt1 last1,
	InputIt2 first2, InputIt2 last2, BinaryPredicate p)
{
	for (; first1 != last1 && first2 != last2; ++first1, ++first2)
	{
		if (!p(*first1, *first2))
			return false;
	}
	return first1 == last1 && first2 == last2;
}

TextEditor::TextEditor()
	: mLineSpacing(1.0f)
	, mUndoIndex(0)
	, mTabSize(4)
	, mOverwrite(false)
	, mReadOnly(false)
	, mWithinRender(false)
	, mScrollToCursor(false)
	, mScrollToTop(false)
	, mTextChanged(false)
	, mColorizerEnabled(true)
	, mTextStart(20.0f)
	, mLeftMargin(10)
	, mCursorPositionChanged(false)
	, mColorRangeMin(0)
	, mColorRangeMax(0)
	, mSelectionMode(SelectionMode::Normal)
	, mCheckComments(true)
	, mLastClick(-1.0f)
	, mHandleKeyboardInputs(true)
	, mHandleMouseInputs(true)
	, mIgnoreImGuiChild(false)
	, mShowWhitespaces(false)
	, mStartTime(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())
{
	SetPalette(GetDarkPalette());
	SetLanguageDefinition(LanguageDefinition::CPlusPlus());
	mLines.push_back(Line());
}

TextEditor::~TextEditor()
{
}

void TextEditor::SetLanguageDefinition(const LanguageDefinition& aLanguageDef)
{
	mLanguageDefinition = aLanguageDef;
	mRegexList.clear();

	for (auto& r : mLanguageDefinition.mTokenRegexStrings)
		mRegexList.push_back(std::make_pair(std::regex(r.first, std::regex_constants::optimize), r.second));

	Colorize();
}

void TextEditor::SetPalette(const Palette& aValue)
{
	mPaletteBase = aValue;
}

std::string TextEditor::GetText(const Coordinates& aStart, const Coordinates& aEnd) const
{
	std::string result;

	auto lstart = aStart.mLine;
	auto lend = aEnd.mLine;
	auto istart = GetCharacterIndex(aStart);
	auto iend = GetCharacterIndex(aEnd)+1;
	size_t s = 0;

	for (size_t i = lstart; i < lend; i++)
		s += mLines[i].size();

	result.reserve(s + s / 8);

	while (istart < iend || lstart < lend)
	{
		if (lstart >= (int)mLines.size())
			break;

		auto& line = mLines[lstart];
		if (istart < (int)line.size())
		{
			result += line[istart].mChar;
			istart++;
		}
		else
		{
			istart = 0;
			++lstart;
			result += '\n';
		}
	}

	result = result.substr(0, result.size() - 1);
	return result;
}

TextEditor::Coordinates TextEditor::GetActualCursorCoordinates() const
{
	return SanitizeCoordinates(mState.mCursorPosition);
}

TextEditor::Coordinates TextEditor::SanitizeCoordinates(const Coordinates& aValue) const
{
	auto line = aValue.mLine;
	auto column = aValue.mColumn;
	if (line >= (int)mLines.size())
	{
		if (mLines.empty())
		{
			line = 0;
			column = 0;
		}
		else
		{
			line = (int)mLines.size() - 1;
			column = GetLineMaxColumn(line);
		}
		return Coordinates(line, column);
	}
	else
	{
		column = mLines.empty() ? 0 : std::min(column, GetLineMaxColumn(line));
		return Coordinates(line, column);
	}
}

// https://en.wikipedia.org/wiki/UTF-8
// We assume that the char is a standalone character (<128) or a leading byte of an UTF-8 code sequence (non-10xxxxxx code)
static int UTF8CharLength(TextEditor::Char c)
{
	if ((c & 0xFE) == 0xFC)
		return 6;
	if ((c & 0xFC) == 0xF8)
		return 5;
	if ((c & 0xF8) == 0xF0)
		return 4;
	else if ((c & 0xF0) == 0xE0)
		return 3;
	else if ((c & 0xE0) == 0xC0)
		return 2;
	return 1;
}

// "Borrowed" from ImGui source
static inline int ImTextCharToUtf8(char* buf, int buf_size, unsigned int c)
{
	if (c < 0x80)
	{
		buf[0] = (char)c;
		return 1;
	}
	if (c < 0x800)
	{
		if (buf_size < 2) return 0;
		buf[0] = (char)(0xc0 + (c >> 6));
		buf[1] = (char)(0x80 + (c & 0x3f));
		return 2;
	}
	if (c >= 0xdc00 && c < 0xe000)
	{
		return 0;
	}
	if (c >= 0xd800 && c < 0xdc00)
	{
		if (buf_size < 4) return 0;
		buf[0] = (char)(0xf0 + (c >> 18));
		buf[1] = (char)(0x80 + ((c >> 12) & 0x3f));
		buf[2] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[3] = (char)(0x80 + ((c) & 0x3f));
		return 4;
	}
	//else if (c < 0x10000)
	{
		if (buf_size < 3) return 0;
		buf[0] = (char)(0xe0 + (c >> 12));
		buf[1] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[2] = (char)(0x80 + ((c) & 0x3f));
		return 3;
	}
}

void TextEditor::Advance(Coordinates& aCoordinates) const
{
	if (aCoordinates.mLine < (int)mLines.size())
	{
		auto& line = mLines[aCoordinates.mLine];
		auto cindex = GetCharacterIndex(aCoordinates);

		if (cindex + 1 < (int)line.size())
		{
			auto delta = UTF8CharLength(line[cindex].mChar);
			cindex = std::min(cindex + delta, (int)line.size() - 1);
		}
		else
		{
			++aCoordinates.mLine;
			cindex = 0;
		}
		aCoordinates.mColumn = GetCharacterColumn(aCoordinates.mLine, cindex);
	}
}

void TextEditor::DeleteRange(const Coordinates& aStart, const Coordinates& aEnd)
{
	assert(aEnd >= aStart);
	assert(!mReadOnly);

	//printf("D(%d.%d)-(%d.%d)\n", aStart.mLine, aStart.mColumn, aEnd.mLine, aEnd.mColumn);

	if (aEnd == aStart)
		return;

	auto start = GetCharacterIndex(aStart);
	auto end = GetCharacterIndex(aEnd);

	if (aStart.mLine == aEnd.mLine)
	{
		auto& line = mLines[aStart.mLine];
		auto n = GetLineMaxColumn(aStart.mLine);
		if (aEnd.mColumn >= n)
			line.erase(line.begin() + start, line.end());
		else
			line.erase(line.begin() + start, line.begin() + end);
	}
	else
	{
		auto& firstLine = mLines[aStart.mLine];
		auto& lastLine = mLines[aEnd.mLine];

		firstLine.erase(firstLine.begin() + start, firstLine.end());
		lastLine.erase(lastLine.begin(), lastLine.begin() + end);

		if (aStart.mLine < aEnd.mLine)
			firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());

		if (aStart.mLine < aEnd.mLine)
			RemoveLine(aStart.mLine + 1, aEnd.mLine + 1);
	}

	mTextChanged = true;
}

int TextEditor::InsertTextAt(Coordinates& /* inout */ aWhere, const char* aValue)
{
	assert(!mReadOnly);

	int cindex = GetCharacterIndex(aWhere);
	int totalLines = 0;
	while (*aValue != '\0')
	{
		assert(!mLines.empty());

		if (*aValue == '\r')
		{
			// skip
			++aValue;
		}
		else if (*aValue == '\n')
		{
			if (cindex < (int)mLines[aWhere.mLine].size())
			{
				auto& newLine = InsertLine(aWhere.mLine + 1);
				auto& line = mLines[aWhere.mLine];
				newLine.insert(newLine.begin(), line.begin() + cindex, line.end());
				line.erase(line.begin() + cindex, line.end());
			}
			else
			{
				InsertLine(aWhere.mLine + 1);
			}
			++aWhere.mLine;
			aWhere.mColumn = 0;
			cindex = 0;
			++totalLines;
			++aValue;
		}
		else
		{
			auto& line = mLines[aWhere.mLine];
			auto d = UTF8CharLength(*aValue);
			while (d-- > 0 && *aValue != '\0')
				line.insert(line.begin() + cindex++, Glyph(*aValue++, PaletteIndex::Default));
			++aWhere.mColumn;
		}

		mTextChanged = true;
	}

	return totalLines;
}

void TextEditor::AddUndo(UndoRecord& aValue)
{
	assert(!mReadOnly);
	//printf("AddUndo: (@%d.%d) +\'%s' [%d.%d .. %d.%d], -\'%s', [%d.%d .. %d.%d] (@%d.%d)\n",
	//	aValue.mBefore.mCursorPosition.mLine, aValue.mBefore.mCursorPosition.mColumn,
	//	aValue.mAdded.c_str(), aValue.mAddedStart.mLine, aValue.mAddedStart.mColumn, aValue.mAddedEnd.mLine, aValue.mAddedEnd.mColumn,
	//	aValue.mRemoved.c_str(), aValue.mRemovedStart.mLine, aValue.mRemovedStart.mColumn, aValue.mRemovedEnd.mLine, aValue.mRemovedEnd.mColumn,
	//	aValue.mAfter.mCursorPosition.mLine, aValue.mAfter.mCursorPosition.mColumn
	//	);

	mUndoBuffer.resize((size_t)(mUndoIndex + 1));
	mUndoBuffer.back() = aValue;
	++mUndoIndex;
}

TextEditor::Coordinates TextEditor::ScreenPosToCoordinates(const ImVec2& aPosition) const
{
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 local(aPosition.x - origin.x, aPosition.y - origin.y);

	int lineNo = std::max(0, (int)floor(local.y / mCharAdvance.y));

	int columnCoord = 0;

	if (lineNo >= 0 && lineNo < (int)mLines.size())
	{
		auto& line = mLines.at(lineNo);

		int columnIndex = 0;
		float columnX = 0.0f;

		while ((size_t)columnIndex < line.size())
		{
			float columnWidth = 0.0f;

			if (line[columnIndex].mChar == '\t')
			{
				float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
				float oldX = columnX;
				float newColumnX = (1.0f + std::floor((1.0f + columnX) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
				columnWidth = newColumnX - oldX;
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
					break;
				columnX = newColumnX;
				columnCoord = (columnCoord / mTabSize) * mTabSize + mTabSize;
				columnIndex++;
			}
			else
			{
				char buf[7];
				auto d = UTF8CharLength(line[columnIndex].mChar);
				int i = 0;
				while (i < 6 && d-- > 0)
					buf[i++] = line[columnIndex++].mChar;
				buf[i] = '\0';
				columnWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf).x;
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
					break;
				columnX += columnWidth;
				columnCoord++;
			}
		}
	}

	return SanitizeCoordinates(Coordinates(lineNo, columnCoord));
}

TextEditor::Coordinates TextEditor::FindWordStart(const Coordinates& aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
		return at;

	while (cindex > 0 && isspace(line[cindex].mChar))
		--cindex;

	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex > 0)
	{
		auto c = line[cindex].mChar;
		if ((c & 0xC0) != 0x80)	// not UTF code sequence 10xxxxxx
		{
			if (c <= 32 && isspace(c))
			{
				cindex++;
				break;
			}
			if (cstart != (PaletteIndex)line[size_t(cindex - 1)].mColorIndex)
				break;
		}
		--cindex;
	}
	return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindWordEnd(const Coordinates& aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
		return at;

	bool prevspace = (bool)isspace(line[cindex].mChar);
	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex < (int)line.size())
	{
		auto c = line[cindex].mChar;
		auto d = UTF8CharLength(c);
		if (cstart != (PaletteIndex)line[cindex].mColorIndex)
			break;

		if (prevspace != !!isspace(c))
		{
			if (isspace(c))
				while (cindex < (int)line.size() && isspace(line[cindex].mChar))
					++cindex;
			break;
		}
		cindex += d;
	}
	return Coordinates(aFrom.mLine, GetCharacterColumn(aFrom.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindNextWord(const Coordinates& aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	// skip to the next non-word character
	auto cindex = GetCharacterIndex(aFrom);
	bool isword = false;
	bool skip = false;
	if (cindex < (int)mLines[at.mLine].size())
	{
		auto& line = mLines[at.mLine];
		isword = isalnum(line[cindex].mChar);
		skip = isword;
	}

	while (!isword || skip)
	{
		if (at.mLine >= mLines.size())
		{
			auto l = std::max(0, (int)mLines.size() - 1);
			return Coordinates(l, GetLineMaxColumn(l));
		}

		auto& line = mLines[at.mLine];
		if (cindex < (int)line.size())
		{
			isword = isalnum(line[cindex].mChar);

			if (isword && !skip)
				return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));

			if (!isword)
				skip = false;

			cindex++;
		}
		else
		{
			cindex = 0;
			++at.mLine;
			skip = false;
			isword = false;
		}
	}

	return at;
}

int TextEditor::GetCharacterIndex(const Coordinates& aCoordinates) const
{
	if (aCoordinates.mLine >= mLines.size())
		return -1;
	auto& line = mLines[aCoordinates.mLine];
	int c = 0;
	int i = 0;
	for (; i < line.size() && c < aCoordinates.mColumn;)
	{
		if (line[i].mChar == '\t')
			c = (c / mTabSize) * mTabSize + mTabSize;
		else
			++c;
		i += UTF8CharLength(line[i].mChar);
	}
	return i;
}

int TextEditor::GetCharacterColumn(int aLine, int aIndex) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int col = 0;
	int i = 0;
	while (i < aIndex && i < (int)line.size())
	{
		auto c = line[i].mChar;
		i += UTF8CharLength(c);
		if (c == '\t')
			col = (col / mTabSize) * mTabSize + mTabSize;
		else
			col++;
	}
	return col;
}

int TextEditor::GetLineCharacterCount(int aLine) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int c = 0;
	for (unsigned i = 0; i < line.size(); c++)
		i += UTF8CharLength(line[i].mChar);
	return c;
}

int TextEditor::GetLineMaxColumn(int aLine) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int col = 0;
	for (unsigned i = 0; i < line.size(); )
	{
		auto c = line[i].mChar;
		if (c == '\t')
			col = (col / mTabSize) * mTabSize + mTabSize;
		else
			col++;
		i += UTF8CharLength(c);
	}
	return col;
}

bool TextEditor::IsOnWordBoundary(const Coordinates& aAt) const
{
	if (aAt.mLine >= (int)mLines.size() || aAt.mColumn == 0)
		return true;

	auto& line = mLines[aAt.mLine];
	auto cindex = GetCharacterIndex(aAt);
	if (cindex >= (int)line.size())
		return true;

	if (mColorizerEnabled)
		return line[cindex].mColorIndex != line[size_t(cindex - 1)].mColorIndex;

	return isspace(line[cindex].mChar) != isspace(line[cindex - 1].mChar);
}

void TextEditor::RemoveLine(int aStart, int aEnd)
{
	assert(!mReadOnly);
	assert(aEnd >= aStart);
	assert(mLines.size() > (size_t)(aEnd - aStart));

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
	{
		ErrorMarkers::value_type e(i.first >= aStart ? i.first - 1 : i.first, i.second);
		if (e.first >= aStart && e.first <= aEnd)
			continue;
		etmp.insert(e);
	}
	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
	{
		if (i >= aStart && i <= aEnd)
			continue;
		btmp.insert(i >= aStart ? i - 1 : i);
	}
	mBreakpoints = std::move(btmp);

	mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
	assert(!mLines.empty());

	mTextChanged = true;
}

void TextEditor::RemoveLine(int aIndex)
{
	assert(!mReadOnly);
	assert(mLines.size() > 1);

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
	{
		ErrorMarkers::value_type e(i.first > aIndex ? i.first - 1 : i.first, i.second);
		if (e.first - 1 == aIndex)
			continue;
		etmp.insert(e);
	}
	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
	{
		if (i == aIndex)
			continue;
		btmp.insert(i >= aIndex ? i - 1 : i);
	}
	mBreakpoints = std::move(btmp);

	mLines.erase(mLines.begin() + aIndex);
	assert(!mLines.empty());

	mTextChanged = true;
}

TextEditor::Line& TextEditor::InsertLine(int aIndex)
{
	assert(!mReadOnly);

	auto& result = *mLines.insert(mLines.begin() + aIndex, Line());

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
		etmp.insert(ErrorMarkers::value_type(i.first >= aIndex ? i.first + 1 : i.first, i.second));
	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
		btmp.insert(i >= aIndex ? i + 1 : i);
	mBreakpoints = std::move(btmp);

	return result;
}

std::string TextEditor::GetWordUnderCursor() const
{
	auto c = GetCursorPosition();
	return GetWordAt(c);
}

std::string TextEditor::GetWordAt(const Coordinates& aCoords) const
{
	auto start = FindWordStart(aCoords);
	auto end = FindWordEnd(aCoords);

	std::string r;

	auto istart = GetCharacterIndex(start);
	auto iend = GetCharacterIndex(end);

	for (auto it = istart; it < iend; ++it)
		r.push_back(mLines[aCoords.mLine][it].mChar);

	return r;
}

ImU32 TextEditor::GetGlyphColor(const Glyph& aGlyph) const
{
	if (!mColorizerEnabled)
		return mPalette[(int)PaletteIndex::Default];
	if (aGlyph.mComment)
		return mPalette[(int)PaletteIndex::Comment];
	if (aGlyph.mMultiLineComment)
		return mPalette[(int)PaletteIndex::MultiLineComment];
	auto const color = mPalette[(int)aGlyph.mColorIndex];
	if (aGlyph.mPreprocessor)
	{
		const auto ppcolor = mPalette[(int)PaletteIndex::Preprocessor];
		const int c0 = ((ppcolor & 0xff) + (color & 0xff)) / 2;
		const int c1 = (((ppcolor >> 8) & 0xff) + ((color >> 8) & 0xff)) / 2;
		const int c2 = (((ppcolor >> 16) & 0xff) + ((color >> 16) & 0xff)) / 2;
		const int c3 = (((ppcolor >> 24) & 0xff) + ((color >> 24) & 0xff)) / 2;
		return ImU32(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24));
	}
	return color;
}

void TextEditor::HandleKeyboardInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	if (ImGui::IsWindowFocused())
	{
		if (ImGui::IsWindowHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		//ImGui::CaptureKeyboardFromApp(true);

		io.WantCaptureKeyboard = true;
		io.WantTextInput = true;

		ErrorMarkers markers;
		SetErrorMarkers(markers);

		if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z)))
			Undo();
		else if (!IsReadOnly() && !ctrl && !shift && alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
			Undo();
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Y)))
			Redo();
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow)))
			MoveUp(1, shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow)))
			MoveDown(1, shift);
		else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_LeftArrow)))
			MoveLeft(1, shift, ctrl);
		else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_RightArrow)))
			MoveRight(1, shift, ctrl);
		else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageUp)))
			MoveUp(GetPageSize() - 4, shift);
		else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageDown)))
			MoveDown(GetPageSize() - 4, shift);
		else if (!alt && ctrl && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
			MoveTop(shift);
		else if (ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
			MoveBottom(shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
			MoveHome(shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
			MoveEnd(shift);
		else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete)))
			Delete();
		else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
			Backspace();
		//else if (!ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
			//mOverwrite ^= true;
		//else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
			///Copy();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_C)))
			Copy();
		else if (!IsReadOnly() && !ctrl && shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
			Paste();
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_V)))
			Paste();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_X)))
			Cut();
		else if (!ctrl && shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete)))
			Cut();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_A)))
			SelectAll();
		else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)))
			EnterCharacter('\n', false);
		else if (!IsReadOnly() && !ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab)))
			EnterCharacter('\t', shift);

		if (!IsReadOnly() && !io.InputQueueCharacters.empty())
		{
			for (int i = 0; i < io.InputQueueCharacters.Size; i++)
			{
				auto c = io.InputQueueCharacters[i];
				if (c != 0 && (c == '\n' || c >= 32))
					EnterCharacter(c, shift);
			}
			io.InputQueueCharacters.resize(0);
		}
	}
}

void TextEditor::HandleMouseInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	if (ImGui::IsWindowHovered())
	{
		if (!shift && !alt)
		{
			auto click = ImGui::IsMouseClicked(0);
			auto doubleClick = ImGui::IsMouseDoubleClicked(0);
			auto t = ImGui::GetTime();
			auto tripleClick = click && !doubleClick && (mLastClick != -1.0f && (t - mLastClick) < io.MouseDoubleClickTime);

			/*
			Left mouse button triple click
			*/

			if (tripleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					mSelectionMode = SelectionMode::Line;
					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = -1.0f;
			}

			/*
			Left mouse button double click
			*/

			else if (doubleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					if (mSelectionMode == SelectionMode::Line)
						mSelectionMode = SelectionMode::Normal;
					else
						mSelectionMode = SelectionMode::Word;
					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = (float)ImGui::GetTime();
			}

			/*
			Left mouse button click
			*/
			else if (click)
			{
				mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				if (ctrl)
					mSelectionMode = SelectionMode::Word;
				else
					mSelectionMode = SelectionMode::Normal;
				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);

				mLastClick = (float)ImGui::GetTime();
			}
			// Mouse left button dragging (=> update selection)
			else if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0))
			{
				io.WantCaptureMouse = true;
				mState.mCursorPosition = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
			}
		}
	}
}

void TextEditor::Render()
{
	/* Compute mCharAdvance regarding to scaled font size (Ctrl + mouse wheel)*/
	const float fontSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr).x;
	mCharAdvance = ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * mLineSpacing);

	/* Update palette with the current alpha from style */
	for (int i = 0; i < (int)PaletteIndex::Max; ++i)
	{
		auto color = ImGui::ColorConvertU32ToFloat4(mPaletteBase[i]);
		color.w *= ImGui::GetStyle().Alpha;
		mPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
	}

	assert(mLineBuffer.empty());

	auto contentSize = ImGui::GetWindowContentRegionMax();
	auto drawList = ImGui::GetWindowDrawList();
	float longest(mTextStart);

	if (mScrollToTop)
	{
		mScrollToTop = false;
		ImGui::SetScrollY(0.f);
	}

	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	auto scrollX = ImGui::GetScrollX();
	auto scrollY = ImGui::GetScrollY();

	auto lineNo = (int)floor(scrollY / mCharAdvance.y);
	auto globalLineMax = (int)mLines.size();
	auto lineMax = std::max(0, std::min((int)mLines.size() - 1, lineNo + (int)floor((scrollY + contentSize.y) / mCharAdvance.y)));

	// Deduce mTextStart by evaluating mLines size (global lineMax) plus two spaces as text width
	char buf[16];
	snprintf(buf, 16, " %d ", globalLineMax);
	mTextStart = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x + mLeftMargin;

	if (!mLines.empty())
	{
		float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

		while (lineNo <= lineMax)
		{
			ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * mCharAdvance.y);
			ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

			auto& line = mLines[lineNo];
			longest = std::max(mTextStart + TextDistanceToLineStart(Coordinates(lineNo, GetLineMaxColumn(lineNo))), longest);
			auto columnNo = 0;
			Coordinates lineStartCoord(lineNo, 0);
			Coordinates lineEndCoord(lineNo, GetLineMaxColumn(lineNo));

			// Draw selection for the current line
			float sstart = -1.0f;
			float ssend = -1.0f;

			assert(mState.mSelectionStart <= mState.mSelectionEnd);
			if (mState.mSelectionStart <= lineEndCoord)
				sstart = mState.mSelectionStart > lineStartCoord ? TextDistanceToLineStart(mState.mSelectionStart) : 0.0f;
			if (mState.mSelectionEnd > lineStartCoord)
				ssend = TextDistanceToLineStart(mState.mSelectionEnd < lineEndCoord ? mState.mSelectionEnd : lineEndCoord);

			if (mState.mSelectionEnd.mLine > lineNo)
				ssend += mCharAdvance.x;

			if (sstart != -1 && ssend != -1 && sstart < ssend)
			{
				ImVec2 vstart(lineStartScreenPos.x + mTextStart + sstart, lineStartScreenPos.y);
				ImVec2 vend(lineStartScreenPos.x + mTextStart + ssend, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Selection]);
			}

			// Draw breakpoints
			auto start = ImVec2(lineStartScreenPos.x + scrollX, lineStartScreenPos.y);

			if (mBreakpoints.count(lineNo + 1) != 0)
			{
				auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::Breakpoint]);
			}

			// Draw error markers
			auto errorIt = mErrorMarkers.find(lineNo + 1);
			if (errorIt != mErrorMarkers.end())
			{
				auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::ErrorMarker]);

				if (ImGui::IsMouseHoveringRect(lineStartScreenPos, end))
				{
					ImGui::BeginTooltip();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
					ImGui::Text("Error at line %d:", errorIt->first);
					ImGui::PopStyleColor();
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
					ImGui::Text("%s", errorIt->second.c_str());
					ImGui::PopStyleColor();
					ImGui::EndTooltip();
				}
			}

			// Draw line number (right aligned)
			snprintf(buf, 16, "%d  ", lineNo + 1);

			auto lineNoWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x;
			drawList->AddText(ImVec2(lineStartScreenPos.x + mTextStart - lineNoWidth, lineStartScreenPos.y), mPalette[(int)PaletteIndex::LineNumber], buf);

			if (mState.mCursorPosition.mLine == lineNo)
			{
				auto focused = ImGui::IsWindowFocused();

				// Highlight the current line (where the cursor is)
				if (!HasSelection())
				{
					auto end = ImVec2(start.x + contentSize.x + scrollX, start.y + mCharAdvance.y);
					drawList->AddRectFilled(start, end, mPalette[(int)(focused ? PaletteIndex::CurrentLineFill : PaletteIndex::CurrentLineFillInactive)]);
					drawList->AddRect(start, end, mPalette[(int)PaletteIndex::CurrentLineEdge], 1.0f);
				}

				// Render the cursor
				if (focused)
				{
					auto timeEnd = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
					auto elapsed = timeEnd - mStartTime;
					if (elapsed > 400)
					{
						float width = 1.0f;
						auto cindex = GetCharacterIndex(mState.mCursorPosition);
						float cx = TextDistanceToLineStart(mState.mCursorPosition);

						if (mOverwrite && cindex < (int)line.size())
						{
							auto c = line[cindex].mChar;
							if (c == '\t')
							{
								auto x = (1.0f + std::floor((1.0f + cx) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
								width = x - cx;
							}
							else
							{
								char buf2[2];
								buf2[0] = line[cindex].mChar;
								buf2[1] = '\0';
								width = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf2).x;
							}
						}
						ImVec2 cstart(textScreenPos.x + cx, lineStartScreenPos.y);
						ImVec2 cend(textScreenPos.x + cx + width, lineStartScreenPos.y + mCharAdvance.y);
						drawList->AddRectFilled(cstart, cend, mPalette[(int)PaletteIndex::Cursor]);
						if (elapsed > 800)
							mStartTime = timeEnd;
					}
				}
			}

			// Render colorized text
			auto prevColor = line.empty() ? mPalette[(int)PaletteIndex::Default] : GetGlyphColor(line[0]);
			ImVec2 bufferOffset;

			for (int i = 0; i < line.size();)
			{
				auto& glyph = line[i];
				auto color = GetGlyphColor(glyph);

				if ((color != prevColor || glyph.mChar == '\t' || glyph.mChar == ' ') && !mLineBuffer.empty())
				{
					const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
					drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
					auto textSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, mLineBuffer.c_str(), nullptr, nullptr);
					bufferOffset.x += textSize.x;
					mLineBuffer.clear();
				}
				prevColor = color;

				if (glyph.mChar == '\t')
				{
					auto oldX = bufferOffset.x;
					bufferOffset.x = (1.0f + std::floor((1.0f + bufferOffset.x) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
					++i;

					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x1 = textScreenPos.x + oldX + 1.0f;
						const auto x2 = textScreenPos.x + bufferOffset.x - 1.0f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						const ImVec2 p1(x1, y);
						const ImVec2 p2(x2, y);
						const ImVec2 p3(x2 - s * 0.2f, y - s * 0.2f);
						const ImVec2 p4(x2 - s * 0.2f, y + s * 0.2f);
						drawList->AddLine(p1, p2, 0x90909090);
						drawList->AddLine(p2, p3, 0x90909090);
						drawList->AddLine(p2, p4, 0x90909090);
					}
				}
				else if (glyph.mChar == ' ')
				{
					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x = textScreenPos.x + bufferOffset.x + spaceSize * 0.5f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
					}
					bufferOffset.x += spaceSize;
					i++;
				}
				else
				{
					auto l = UTF8CharLength(glyph.mChar);
					while (l-- > 0)
						mLineBuffer.push_back(line[i++].mChar);
				}
				++columnNo;
			}

			if (!mLineBuffer.empty())
			{
				const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
				drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
				mLineBuffer.clear();
			}

			++lineNo;
		}

		// Draw a tooltip on known identifiers/preprocessor symbols
		if (ImGui::IsMousePosValid() && ImGui::IsWindowHovered())
		{
			auto id = GetWordAt(ScreenPosToCoordinates(ImGui::GetMousePos()));
			if (!id.empty())
			{
				auto it = mLanguageDefinition.mIdentifiers.find(id);
				if (it != mLanguageDefinition.mIdentifiers.end())
				{
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(it->second.mDeclaration.c_str());
					ImGui::EndTooltip();
				}
				else
				{
					auto pi = mLanguageDefinition.mPreprocIdentifiers.find(id);
					if (pi != mLanguageDefinition.mPreprocIdentifiers.end())
					{
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(pi->second.mDeclaration.c_str());
						ImGui::EndTooltip();
					}
				}
			}
		}
	}


	ImGui::Dummy(ImVec2((longest + 2), mLines.size() * mCharAdvance.y));

	if (mScrollToCursor)
	{
		EnsureCursorVisible();
		ImGui::SetWindowFocus();
		mScrollToCursor = false;
	}
}

void TextEditor::Render(const char* aTitle, const ImVec2& aSize, bool aBorder)
{
	mWithinRender = true;
	mTextChanged = false;
	mCursorPositionChanged = false;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	if (!mIgnoreImGuiChild)
		ImGui::BeginChild(aTitle, aSize, aBorder, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar);

	if (mHandleKeyboardInputs)
	{
		HandleKeyboardInputs();
		ImGui::PushAllowKeyboardFocus(true);
	}

	if (mHandleMouseInputs)
		HandleMouseInputs();

	ColorizeInternal();
	Render();

	if (mHandleKeyboardInputs)
		ImGui::PopAllowKeyboardFocus();

	if (!mIgnoreImGuiChild)
		ImGui::EndChild();

	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	mWithinRender = false;
}

void TextEditor::SetText(const std::string& aText)
{
	mLines.clear();
	mLines.emplace_back(Line());
	for (auto chr : aText)
	{
		if (chr == '\r')
		{
			// ignore the carriage return character
		}
		else if (chr == '\n')
			mLines.emplace_back(Line());
		else
		{
			mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void TextEditor::SetTextLines(const std::vector<std::string>& aLines)
{
	mLines.clear();

	if (aLines.empty())
	{
		mLines.emplace_back(Line());
	}
	else
	{
		mLines.resize(aLines.size());

		for (size_t i = 0; i < aLines.size(); ++i)
		{
			const std::string& aLine = aLines[i];

			mLines[i].reserve(aLine.size());
			for (size_t j = 0; j < aLine.size(); ++j)
				mLines[i].emplace_back(Glyph(aLine[j], PaletteIndex::Default));
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void TextEditor::EnterCharacter(ImWchar aChar, bool aShift)
{
	assert(!mReadOnly);

	UndoRecord u;

	u.mBefore = mState;

	if (HasSelection())
	{
		if (aChar == '\t' && mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine)
		{

			auto start = mState.mSelectionStart;
			auto end = mState.mSelectionEnd;
			auto originalEnd = end;

			if (start > end)
				std::swap(start, end);
			start.mColumn = 0;
			//			end.mColumn = end.mLine < mLines.size() ? mLines[end.mLine].size() : 0;
			if (end.mColumn == 0 && end.mLine > 0)
				--end.mLine;
			if (end.mLine >= (int)mLines.size())
				end.mLine = mLines.empty() ? 0 : (int)mLines.size() - 1;
			end.mColumn = GetLineMaxColumn(end.mLine);

			//if (end.mColumn >= GetLineMaxColumn(end.mLine))
			//	end.mColumn = GetLineMaxColumn(end.mLine) - 1;

			u.mRemovedStart = start;
			u.mRemovedEnd = end;
			u.mRemoved = GetText(start, end);

			bool modified = false;

			for (int i = start.mLine; i <= end.mLine; i++)
			{
				auto& line = mLines[i];
				if (aShift)
				{
					if (!line.empty())
					{
						if (line.front().mChar == '\t')
						{
							line.erase(line.begin());
							modified = true;
						}
						else
						{
							for (int j = 0; j < mTabSize && !line.empty() && line.front().mChar == ' '; j++)
							{
								line.erase(line.begin());
								modified = true;
							}
						}
					}
				}
				else
				{
					line.insert(line.begin(), Glyph('\t', TextEditor::PaletteIndex::Background));
					modified = true;
				}
			}

			if (modified)
			{
				start = Coordinates(start.mLine, GetCharacterColumn(start.mLine, 0));
				Coordinates rangeEnd;
				if (originalEnd.mColumn != 0)
				{
					end = Coordinates(end.mLine, GetLineMaxColumn(end.mLine));
					rangeEnd = end;
					u.mAdded = GetText(start, end);
				}
				else
				{
					end = Coordinates(originalEnd.mLine, 0);
					rangeEnd = Coordinates(end.mLine - 1, GetLineMaxColumn(end.mLine - 1));
					u.mAdded = GetText(start, rangeEnd);
				}

				u.mAddedStart = start;
				u.mAddedEnd = rangeEnd;
				u.mAfter = mState;

				mState.mSelectionStart = start;
				mState.mSelectionEnd = end;
				AddUndo(u);

				mTextChanged = true;

				EnsureCursorVisible();
			}

			return;
		} // c == '\t'
		else
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}
	} // HasSelection

	auto coord = GetActualCursorCoordinates();
	u.mAddedStart = coord;

	assert(!mLines.empty());

	if (aChar == '\n')
	{
		InsertLine(coord.mLine + 1);
		auto& line = mLines[coord.mLine];
		auto& newLine = mLines[coord.mLine + 1];

		if (mLanguageDefinition.mAutoIndentation)
			for (size_t it = 0; it < line.size() && isascii(line[it].mChar) && isblank(line[it].mChar); ++it)
				newLine.push_back(line[it]);

		const size_t whitespaceSize = newLine.size();
		auto cindex = GetCharacterIndex(coord);
		newLine.insert(newLine.end(), line.begin() + cindex, line.end());
		line.erase(line.begin() + cindex, line.begin() + line.size());
		SetCursorPosition(Coordinates(coord.mLine + 1, GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)));
		u.mAdded = (char)aChar;
	}
	else
	{
		char buf[7];
		int e = ImTextCharToUtf8(buf, 7, aChar);
		if (e > 0)
		{
			buf[e] = '\0';
			auto& line = mLines[coord.mLine];
			auto cindex = GetCharacterIndex(coord);

			if (mOverwrite && cindex < (int)line.size())
			{
				auto d = UTF8CharLength(line[cindex].mChar);

				u.mRemovedStart = mState.mCursorPosition;
				u.mRemovedEnd = Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex + d));

				while (d-- > 0 && cindex < (int)line.size())
				{
					u.mRemoved += line[cindex].mChar;
					line.erase(line.begin() + cindex);
				}
			}

			for (auto p = buf; *p != '\0'; p++, ++cindex)
				line.insert(line.begin() + cindex, Glyph(*p, PaletteIndex::Default));
			u.mAdded = buf;

			SetCursorPosition(Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)));
		}
		else
			return;
	}

	mTextChanged = true;

	u.mAddedEnd = GetActualCursorCoordinates();
	u.mAfter = mState;

	AddUndo(u);

	Colorize(coord.mLine - 1, 3);
	EnsureCursorVisible();
}

void TextEditor::SetReadOnly(bool aValue)
{
	mReadOnly = aValue;
}

void TextEditor::SetColorizerEnable(bool aValue)
{
	mColorizerEnabled = aValue;
}

void TextEditor::SetCursorPosition(const Coordinates& aPosition)
{
	if (mState.mCursorPosition != aPosition)
	{
		mState.mCursorPosition = aPosition;
		mCursorPositionChanged = true;
		EnsureCursorVisible();
	}
}

void TextEditor::SetSelectionStart(const Coordinates& aPosition)
{
	mState.mSelectionStart = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void TextEditor::SetSelectionEnd(const Coordinates& aPosition)
{
	mState.mSelectionEnd = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void TextEditor::SetSelection(const Coordinates& aStart, const Coordinates& aEnd, SelectionMode aMode)
{
	auto oldSelStart = mState.mSelectionStart;
	auto oldSelEnd = mState.mSelectionEnd;

	mState.mSelectionStart = SanitizeCoordinates(aStart);
	mState.mSelectionEnd = SanitizeCoordinates(aEnd);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);

	switch (aMode)
	{
	case TextEditor::SelectionMode::Normal:
		break;
	case TextEditor::SelectionMode::Word:
	{
		mState.mSelectionStart = FindWordStart(mState.mSelectionStart);
		if (!IsOnWordBoundary(mState.mSelectionEnd))
			mState.mSelectionEnd = FindWordEnd(FindWordStart(mState.mSelectionEnd));
		break;
	}
	case TextEditor::SelectionMode::Line:
	{
		const auto lineNo = mState.mSelectionEnd.mLine;
		const auto lineSize = (size_t)lineNo < mLines.size() ? mLines[lineNo].size() : 0;
		mState.mSelectionStart = Coordinates(mState.mSelectionStart.mLine, 0);
		mState.mSelectionEnd = Coordinates(lineNo, GetLineMaxColumn(lineNo));
		break;
	}
	default:
		break;
	}

	if (mState.mSelectionStart != oldSelStart ||
		mState.mSelectionEnd != oldSelEnd)
		mCursorPositionChanged = true;
}

void TextEditor::SetTabSize(int aValue)
{
	mTabSize = std::max(0, std::min(32, aValue));
}

void TextEditor::InsertText(const std::string& aValue)
{
	InsertText(aValue.c_str());
}

void TextEditor::InsertText(const char* aValue)
{
	if (aValue == nullptr)
		return;

	auto pos = GetActualCursorCoordinates();
	auto start = std::min(pos, mState.mSelectionStart);
	int totalLines = pos.mLine - start.mLine;

	totalLines += InsertTextAt(pos, aValue);

	SetSelection(pos, pos);
	SetCursorPosition(pos);
	Colorize(start.mLine - 1, totalLines + 2);
}

void TextEditor::DeleteSelection()
{
	assert(mState.mSelectionEnd >= mState.mSelectionStart);

	if (mState.mSelectionEnd == mState.mSelectionStart)
		return;

	DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);

	SetSelection(mState.mSelectionStart, mState.mSelectionStart);
	SetCursorPosition(mState.mSelectionStart);
	Colorize(mState.mSelectionStart.mLine, 1);
}

void TextEditor::MoveUp(int aAmount, bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, mState.mCursorPosition.mLine - aAmount);
	if (oldPos != mState.mCursorPosition)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

void TextEditor::MoveDown(int aAmount, bool aSelect)
{
	assert(mState.mCursorPosition.mColumn >= 0);
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + aAmount));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

static bool IsUTFSequence(char c)
{
	return (c & 0xC0) == 0x80;
}

void TextEditor::MoveLeft(int aAmount, bool aSelect, bool aWordMode)
{
	if (mLines.empty())
		return;

	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition = GetActualCursorCoordinates();
	auto line = mState.mCursorPosition.mLine;
	auto cindex = GetCharacterIndex(mState.mCursorPosition);

	while (aAmount-- > 0)
	{
		if (cindex == 0)
		{
			if (line > 0)
			{
				--line;
				if ((int)mLines.size() > line)
					cindex = (int)mLines[line].size();
				else
					cindex = 0;
			}
		}
		else
		{
			--cindex;
			if (cindex > 0)
			{
				if ((int)mLines.size() > line)
				{
					while (cindex > 0 && IsUTFSequence(mLines[line][cindex].mChar))
						--cindex;
				}
			}
		}

		mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));
		if (aWordMode)
		{
			mState.mCursorPosition = FindWordStart(mState.mCursorPosition);
			cindex = GetCharacterIndex(mState.mCursorPosition);
		}
	}

	mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));

	assert(mState.mCursorPosition.mColumn >= 0);
	if (aSelect)
	{
		if (oldPos == mInteractiveStart)
			mInteractiveStart = mState.mCursorPosition;
		else if (oldPos == mInteractiveEnd)
			mInteractiveEnd = mState.mCursorPosition;
		else
		{
			mInteractiveStart = mState.mCursorPosition;
			mInteractiveEnd = oldPos;
		}
	}
	else
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void TextEditor::MoveRight(int aAmount, bool aSelect, bool aWordMode)
{
	auto oldPos = mState.mCursorPosition;

	if (mLines.empty() || oldPos.mLine >= mLines.size())
		return;

	auto cindex = GetCharacterIndex(mState.mCursorPosition);
	while (aAmount-- > 0)
	{
		auto lindex = mState.mCursorPosition.mLine;
		auto& line = mLines[lindex];

		if (cindex >= line.size())
		{
			if (mState.mCursorPosition.mLine < mLines.size() - 1)
			{
				mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + 1));
				mState.mCursorPosition.mColumn = 0;
			}
			else
				return;
		}
		else
		{
			cindex += UTF8CharLength(line[cindex].mChar);
			mState.mCursorPosition = Coordinates(lindex, GetCharacterColumn(lindex, cindex));
			if (aWordMode)
				mState.mCursorPosition = FindNextWord(mState.mCursorPosition);
		}
	}

	if (aSelect)
	{
		if (oldPos == mInteractiveEnd)
			mInteractiveEnd = SanitizeCoordinates(mState.mCursorPosition);
		else if (oldPos == mInteractiveStart)
			mInteractiveStart = mState.mCursorPosition;
		else
		{
			mInteractiveStart = oldPos;
			mInteractiveEnd = mState.mCursorPosition;
		}
	}
	else
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void TextEditor::MoveTop(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(0, 0));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			mInteractiveEnd = oldPos;
			mInteractiveStart = mState.mCursorPosition;
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::TextEditor::MoveBottom(bool aSelect)
{
	auto oldPos = GetCursorPosition();
	auto newPos = Coordinates((int)mLines.size() - 1, 0);
	SetCursorPosition(newPos);
	if (aSelect)
	{
		mInteractiveStart = oldPos;
		mInteractiveEnd = newPos;
	}
	else
		mInteractiveStart = mInteractiveEnd = newPos;
	SetSelection(mInteractiveStart, mInteractiveEnd);
}

void TextEditor::MoveHome(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, 0));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::MoveEnd(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, GetLineMaxColumn(oldPos.mLine)));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::Delete()
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);
		auto& line = mLines[pos.mLine];

		if (pos.mColumn == GetLineMaxColumn(pos.mLine))
		{
			if (pos.mLine == (int)mLines.size() - 1)
				return;

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			Advance(u.mRemovedEnd);

			auto& nextLine = mLines[pos.mLine + 1];
			line.insert(line.end(), nextLine.begin(), nextLine.end());
			RemoveLine(pos.mLine + 1);
		}
		else
		{
			auto cindex = GetCharacterIndex(pos);
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			u.mRemovedEnd.mColumn++;
			u.mRemoved = GetText(u.mRemovedStart, u.mRemovedEnd);

			auto d = UTF8CharLength(line[cindex].mChar);
			while (d-- > 0 && cindex < (int)line.size())
				line.erase(line.begin() + cindex);
		}

		mTextChanged = true;

		Colorize(pos.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::Backspace()
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);

		if (mState.mCursorPosition.mColumn == 0)
		{
			if (mState.mCursorPosition.mLine == 0)
				return;

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = Coordinates(pos.mLine - 1, GetLineMaxColumn(pos.mLine - 1));
			Advance(u.mRemovedEnd);

			auto& line = mLines[mState.mCursorPosition.mLine];
			auto& prevLine = mLines[mState.mCursorPosition.mLine - 1];
			auto prevSize = GetLineMaxColumn(mState.mCursorPosition.mLine - 1);
			prevLine.insert(prevLine.end(), line.begin(), line.end());

			ErrorMarkers etmp;
			for (auto& i : mErrorMarkers)
				etmp.insert(ErrorMarkers::value_type(i.first - 1 == mState.mCursorPosition.mLine ? i.first - 1 : i.first, i.second));
			mErrorMarkers = std::move(etmp);

			RemoveLine(mState.mCursorPosition.mLine);
			--mState.mCursorPosition.mLine;
			mState.mCursorPosition.mColumn = prevSize;
		}
		else
		{
			auto& line = mLines[mState.mCursorPosition.mLine];
			auto cindex = GetCharacterIndex(pos) - 1;
			auto cend = cindex + 1;
			while (cindex > 0 && IsUTFSequence(line[cindex].mChar))
				--cindex;

			//if (cindex > 0 && UTF8CharLength(line[cindex].mChar) > 1)
			//	--cindex;

			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			--u.mRemovedStart.mColumn;
			--mState.mCursorPosition.mColumn;

			while (cindex < line.size() && cend-- > cindex)
			{
				u.mRemoved += line[cindex].mChar;
				line.erase(line.begin() + cindex);
			}
		}

		mTextChanged = true;

		EnsureCursorVisible();
		Colorize(mState.mCursorPosition.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::SelectWordUnderCursor()
{
	auto c = GetCursorPosition();
	SetSelection(FindWordStart(c), FindWordEnd(c));
}

void TextEditor::SelectAll()
{
	SetSelection(Coordinates(0, 0), Coordinates((int)mLines.size(), 0));
}

bool TextEditor::HasSelection() const
{
	return mState.mSelectionEnd > mState.mSelectionStart;
}

void TextEditor::Copy()
{
	if (HasSelection())
	{
		ImGui::SetClipboardText(GetSelectedText().c_str());
	}
	else
	{
		if (!mLines.empty())
		{
			std::string str;
			auto& line = mLines[GetActualCursorCoordinates().mLine];
			for (auto& g : line)
				str.push_back(g.mChar);
			ImGui::SetClipboardText(str.c_str());
		}
	}
}

void TextEditor::Cut()
{
	if (IsReadOnly())
	{
		Copy();
	}
	else
	{
		if (HasSelection())
		{
			UndoRecord u;
			u.mBefore = mState;
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;

			Copy();
			DeleteSelection();

			u.mAfter = mState;
			AddUndo(u);
		}
	}
}

void TextEditor::Paste()
{
	if (IsReadOnly())
		return;

	auto clipText = ImGui::GetClipboardText();
	if (clipText != nullptr && strlen(clipText) > 0)
	{
		UndoRecord u;
		u.mBefore = mState;

		if (HasSelection())
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}

		u.mAdded = clipText;
		u.mAddedStart = GetActualCursorCoordinates();

		InsertText(clipText);

		u.mAddedEnd = GetActualCursorCoordinates();
		u.mAfter = mState;
		AddUndo(u);
	}
}

bool TextEditor::CanUndo() const
{
	return !mReadOnly && mUndoIndex > 0;
}

bool TextEditor::CanRedo() const
{
	return !mReadOnly && mUndoIndex < (int)mUndoBuffer.size();
}

void TextEditor::Undo(int aSteps)
{
	while (CanUndo() && aSteps-- > 0)
		mUndoBuffer[--mUndoIndex].Undo(this);
}

void TextEditor::Redo(int aSteps)
{
	while (CanRedo() && aSteps-- > 0)
		mUndoBuffer[mUndoIndex++].Redo(this);
}

const TextEditor::Palette& TextEditor::GetDarkPalette()
{
	const static Palette p = { {
			0xff7f7f7f,	// Default
			0xffd69c56,	// Keyword	
			0xff00ff00,	// Number
			0xff7070e0,	// String
			0xff70a0e0, // Char literal
			0xffffffff, // Punctuation
			0xff408080,	// Preprocessor
			0xffaaaaaa, // Identifier
			0xff9bc64d, // Known identifier
			0xffc040a0, // Preproc identifier
			0xff206020, // Comment (single line)
			0xff406020, // Comment (multi line)
			0xff101010, // Background
			0xffe0e0e0, // Cursor
			0x80a06020, // Selection
			0x800020ff, // ErrorMarker
			0x40f08000, // Breakpoint
			0xff707000, // Line number
			0x40000000, // Current line fill
			0x40808080, // Current line fill (inactive)
			0x40a0a0a0, // Current line edge
		} };
	return p;
}

const TextEditor::Palette& TextEditor::GetLightPalette()
{
	const static Palette p = { {
			0xff7f7f7f,	// None
			0xffff0c06,	// Keyword	
			0xff008000,	// Number
			0xff2020a0,	// String
			0xff304070, // Char literal
			0xff000000, // Punctuation
			0xff406060,	// Preprocessor
			0xff404040, // Identifier
			0xff606010, // Known identifier
			0xffc040a0, // Preproc identifier
			0xff205020, // Comment (single line)
			0xff405020, // Comment (multi line)
			0xffffffff, // Background
			0xff000000, // Cursor
			0x80600000, // Selection
			0xa00010ff, // ErrorMarker
			0x80f08000, // Breakpoint
			0xff505000, // Line number
			0x40000000, // Current line fill
			0x40808080, // Current line fill (inactive)
			0x40000000, // Current line edge
		} };
	return p;
}

const TextEditor::Palette& TextEditor::GetRetroBluePalette()
{
	const static Palette p = { {
			0xff00ffff,	// None
			0xffffff00,	// Keyword	
			0xff00ff00,	// Number
			0xff808000,	// String
			0xff808000, // Char literal
			0xffffffff, // Punctuation
			0xff008000,	// Preprocessor
			0xff00ffff, // Identifier
			0xffffffff, // Known identifier
			0xffff00ff, // Preproc identifier
			0xff808080, // Comment (single line)
			0xff404040, // Comment (multi line)
			0xff800000, // Background
			0xff0080ff, // Cursor
			0x80ffff00, // Selection
			0xa00000ff, // ErrorMarker
			0x80ff8000, // Breakpoint
			0xff808000, // Line number
			0x40000000, // Current line fill
			0x40808080, // Current line fill (inactive)
			0x40000000, // Current line edge
		} };
	return p;
}


std::string TextEditor::GetText() const
{
	return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::vector<std::string> TextEditor::GetTextLines() const
{
	std::vector<std::string> result;

	result.reserve(mLines.size());

	for (auto& line : mLines)
	{
		std::string text;

		text.resize(line.size());

		for (size_t i = 0; i < line.size(); ++i)
			text[i] = line[i].mChar;

		result.emplace_back(std::move(text));
	}

	return result;
}

std::string TextEditor::GetSelectedText() const
{
	return GetText(mState.mSelectionStart, mState.mSelectionEnd);
}

std::string TextEditor::GetCurrentLineText()const
{
	auto lineLength = GetLineMaxColumn(mState.mCursorPosition.mLine);
	return GetText(
		Coordinates(mState.mCursorPosition.mLine, 0),
		Coordinates(mState.mCursorPosition.mLine, lineLength));
}

void TextEditor::ProcessInputs()
{
}

void TextEditor::Colorize(int aFromLine, int aLines)
{
	int toLine = aLines == -1 ? (int)mLines.size() : std::min((int)mLines.size(), aFromLine + aLines);
	mColorRangeMin = std::min(mColorRangeMin, aFromLine);
	mColorRangeMax = std::max(mColorRangeMax, toLine);
	mColorRangeMin = std::max(0, mColorRangeMin);
	mColorRangeMax = std::max(mColorRangeMin, mColorRangeMax);
	mCheckComments = true;
}

void TextEditor::ColorizeRange(int aFromLine, int aToLine)
{
	if (mLines.empty() || aFromLine >= aToLine)
		return;

	std::string buffer;
	std::cmatch results;
	std::string id;

	int endLine = std::max(0, std::min((int)mLines.size(), aToLine));
	for (int i = aFromLine; i < endLine; ++i)
	{
		auto& line = mLines[i];

		if (line.empty())
			continue;

		buffer.resize(line.size());
		for (size_t j = 0; j < line.size(); ++j)
		{
			auto& col = line[j];
			buffer[j] = col.mChar;
			col.mColorIndex = PaletteIndex::Default;
		}

		const char* bufferBegin = &buffer.front();
		const char* bufferEnd = bufferBegin + buffer.size();

		auto last = bufferEnd;

		for (auto first = bufferBegin; first != last; )
		{
			const char* token_begin = nullptr;
			const char* token_end = nullptr;
			PaletteIndex token_color = PaletteIndex::Default;

			bool hasTokenizeResult = false;

			if (mLanguageDefinition.mTokenize != nullptr)
			{
				if (mLanguageDefinition.mTokenize(first, last, token_begin, token_end, token_color))
					hasTokenizeResult = true;
			}

			if (hasTokenizeResult == false)
			{
				// todo : remove
				//printf("using regex for %.*s\n", first + 10 < last ? 10 : int(last - first), first);

				for (auto& p : mRegexList)
				{
					if (std::regex_search(first, last, results, p.first, std::regex_constants::match_continuous))
					{
						hasTokenizeResult = true;

						auto& v = *results.begin();
						token_begin = v.first;
						token_end = v.second;
						token_color = p.second;
						break;
					}
				}
			}

			if (hasTokenizeResult == false)
			{
				first++;
			}
			else
			{
				const size_t token_length = token_end - token_begin;

				if (token_color == PaletteIndex::Identifier)
				{
					id.assign(token_begin, token_end);

					// todo : allmost all language definitions use lower case to specify keywords, so shouldn't this use ::tolower ?
					if (!mLanguageDefinition.mCaseSensitive)
						std::transform(id.begin(), id.end(), id.begin(), ::toupper);

					if (!line[first - bufferBegin].mPreprocessor)
					{
						if (mLanguageDefinition.mKeywords.count(id) != 0)
							token_color = PaletteIndex::Keyword;
						else if (mLanguageDefinition.mIdentifiers.count(id) != 0)
							token_color = PaletteIndex::KnownIdentifier;
						else if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
					else
					{
						if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
				}

				for (size_t j = 0; j < token_length; ++j)
					line[(token_begin - bufferBegin) + j].mColorIndex = token_color;

				first = token_end;
			}
		}
	}
}

void TextEditor::ColorizeInternal()
{
	if (mLines.empty() || !mColorizerEnabled)
		return;

	if (mCheckComments)
	{
		auto endLine = mLines.size();
		auto endIndex = 0;
		auto commentStartLine = endLine;
		auto commentStartIndex = endIndex;
		auto withinString = false;
		auto withinSingleLineComment = false;
		auto withinPreproc = false;
		auto firstChar = true;			// there is no other non-whitespace characters in the line before
		auto concatenate = false;		// '\' on the very end of the line
		auto currentLine = 0;
		auto currentIndex = 0;
		while (currentLine < endLine || currentIndex < endIndex)
		{
			auto& line = mLines[currentLine];

			if (currentIndex == 0 && !concatenate)
			{
				withinSingleLineComment = false;
				withinPreproc = false;
				firstChar = true;
			}

			concatenate = false;

			if (!line.empty())
			{
				auto& g = line[currentIndex];
				auto c = g.mChar;

				if (c != mLanguageDefinition.mPreprocChar && !isspace(c))
					firstChar = false;

				if (currentIndex == (int)line.size() - 1 && line[line.size() - 1].mChar == '\\')
					concatenate = true;

				bool inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

				if (withinString)
				{
					line[currentIndex].mMultiLineComment = inComment;

					if (c == '\"')
					{
						if (currentIndex + 1 < (int)line.size() && line[currentIndex + 1].mChar == '\"')
						{
							currentIndex += 1;
							if (currentIndex < (int)line.size())
								line[currentIndex].mMultiLineComment = inComment;
						}
						else
							withinString = false;
					}
					else if (c == '\\')
					{
						currentIndex += 1;
						if (currentIndex < (int)line.size())
							line[currentIndex].mMultiLineComment = inComment;
					}
				}
				else
				{
					if (firstChar && c == mLanguageDefinition.mPreprocChar)
						withinPreproc = true;

					if (c == '\"')
					{
						withinString = true;
						line[currentIndex].mMultiLineComment = inComment;
					}
					else
					{
						auto pred = [](const char& a, const Glyph& b) { return a == b.mChar; };
						auto from = line.begin() + currentIndex;
						auto& startStr = mLanguageDefinition.mCommentStart;
						auto& startStr2 = mLanguageDefinition.mCommentStart2;
						auto& singleStartStr = mLanguageDefinition.mSingleLineComment;
						auto& singleStartStr2 = mLanguageDefinition.mSingleLineComment2;

						if (singleStartStr.size() > 0 &&
							currentIndex + singleStartStr.size() <= line.size() &&
							equals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred) &&
							!equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							withinSingleLineComment = true;
							if (currentIndex + startStr.size() > line.size())
							{
								withinSingleLineComment = true;
							}
							else if (!equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
							{
								withinSingleLineComment = true;
							}
						}
						if (singleStartStr2.size() > 0 &&
							currentIndex + singleStartStr2.size() <= line.size() &&
							equals(singleStartStr2.begin(), singleStartStr2.end(), from, from + singleStartStr2.size(), pred) &&
							!equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							withinSingleLineComment = true;
							if (currentIndex + startStr.size() > line.size())
							{
								withinSingleLineComment = true;
							}
							else if (!equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
							{
								withinSingleLineComment = true;
							}
						}
						if (!withinSingleLineComment && currentIndex + startStr.size() <= line.size() &&
							equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							commentStartLine = currentLine;
							commentStartIndex = currentIndex;
						}

						// AUIWFVBAWUFVAWUFVBAWIFAWIUAWIUGVAWUIFVBAWUIFBAWIUOFBAWUIFGVBAWUFBAWBFGAUIG

						if (singleStartStr.size() > 0 &&
							currentIndex + singleStartStr.size() <= line.size() &&
							equals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred) &&
							!equals(startStr2.begin(), startStr2.end(), from, from + startStr2.size(), pred))
						{
							withinSingleLineComment = true;
							if (currentIndex + startStr2.size() > line.size())
							{
								withinSingleLineComment = true;
							}
							else if (!equals(startStr2.begin(), startStr2.end(), from, from + startStr2.size(), pred))
							{
								withinSingleLineComment = true;
							}
						}
						if (singleStartStr2.size() > 0 &&
							currentIndex + singleStartStr2.size() <= line.size() &&
							equals(singleStartStr2.begin(), singleStartStr2.end(), from, from + singleStartStr2.size(), pred) &&
							!equals(startStr2.begin(), startStr2.end(), from, from + startStr2.size(), pred))
						{
							withinSingleLineComment = true;
							if (currentIndex + startStr2.size() > line.size())
							{
								withinSingleLineComment = true;
							}
							else if (!equals(startStr2.begin(), startStr2.end(), from, from + startStr2.size(), pred))
							{
								withinSingleLineComment = true;
							}
						}
						if (!withinSingleLineComment && currentIndex + startStr2.size() <= line.size() &&
							equals(startStr2.begin(), startStr2.end(), from, from + startStr2.size(), pred))
						{
							commentStartLine = currentLine;
							commentStartIndex = currentIndex;
						}

						inComment = inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

						line[currentIndex].mMultiLineComment = inComment;
						line[currentIndex].mComment = withinSingleLineComment;

						auto& endStr = mLanguageDefinition.mCommentEnd;
						if (currentIndex + 1 >= (int)endStr.size() &&
							equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred))
						{
							commentStartIndex = endIndex;
							commentStartLine = endLine;
						}

						auto& endStr2 = mLanguageDefinition.mCommentEnd2;
						if (currentIndex + 1 >= (int)endStr2.size() &&
							equals(endStr2.begin(), endStr2.end(), from + 1 - endStr2.size(), from + 1, pred))
						{
							commentStartIndex = endIndex;
							commentStartLine = endLine;
						}
					}
				}
				line[currentIndex].mPreprocessor = withinPreproc;
				currentIndex += UTF8CharLength(c);
				if (currentIndex >= (int)line.size())
				{
					currentIndex = 0;
					++currentLine;
				}
			}
			else
			{
				currentIndex = 0;
				++currentLine;
			}
		}
		mCheckComments = false;
	}

	if (mColorRangeMin < mColorRangeMax)
	{
		const int increment = (mLanguageDefinition.mTokenize == nullptr) ? 10 : 10000;
		const int to = std::min(mColorRangeMin + increment, mColorRangeMax);
		ColorizeRange(mColorRangeMin, to);
		mColorRangeMin = to;

		if (mColorRangeMax == mColorRangeMin)
		{
			mColorRangeMin = std::numeric_limits<int>::max();
			mColorRangeMax = 0;
		}
		return;
	}
}

float TextEditor::TextDistanceToLineStart(const Coordinates& aFrom) const
{
	auto& line = mLines[aFrom.mLine];
	float distance = 0.0f;
	float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;
	int colIndex = GetCharacterIndex(aFrom);
	for (size_t it = 0u; it < line.size() && it < colIndex; )
	{
		if (line[it].mChar == '\t')
		{
			distance = (1.0f + std::floor((1.0f + distance) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
			++it;
		}
		else
		{
			auto d = UTF8CharLength(line[it].mChar);
			char tempCString[7];
			int i = 0;
			for (; i < 6 && d-- > 0 && it < (int)line.size(); i++, it++)
				tempCString[i] = line[it].mChar;

			tempCString[i] = '\0';
			distance += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, tempCString, nullptr, nullptr).x;
		}
	}

	return distance;
}

void TextEditor::EnsureCursorVisible()
{
	if (!mWithinRender)
	{
		mScrollToCursor = true;
		return;
	}

	float scrollX = ImGui::GetScrollX();
	float scrollY = ImGui::GetScrollY();

	auto height = ImGui::GetWindowHeight();
	auto width = ImGui::GetWindowWidth();

	auto top = 1 + (int)ceil(scrollY / mCharAdvance.y);
	auto bottom = (int)ceil((scrollY + height) / mCharAdvance.y);

	auto left = (int)ceil(scrollX / mCharAdvance.x);
	auto right = (int)ceil((scrollX + width) / mCharAdvance.x);

	auto pos = GetActualCursorCoordinates();
	auto len = TextDistanceToLineStart(pos);

	if (pos.mLine < top)
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine - 1) * mCharAdvance.y));
	if (pos.mLine > bottom - 4)
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine + 4) * mCharAdvance.y - height));
	if (len + mTextStart < left + 4)
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart - 4));
	if (len + mTextStart > right - 4)
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart + 4 - width));
}

int TextEditor::GetPageSize() const
{
	auto height = ImGui::GetWindowHeight() - 20.0f;
	return (int)floor(height / mCharAdvance.y);
}

TextEditor::UndoRecord::UndoRecord(
	const std::string& aAdded,
	const TextEditor::Coordinates aAddedStart,
	const TextEditor::Coordinates aAddedEnd,
	const std::string& aRemoved,
	const TextEditor::Coordinates aRemovedStart,
	const TextEditor::Coordinates aRemovedEnd,
	TextEditor::EditorState& aBefore,
	TextEditor::EditorState& aAfter)
	: mAdded(aAdded)
	, mAddedStart(aAddedStart)
	, mAddedEnd(aAddedEnd)
	, mRemoved(aRemoved)
	, mRemovedStart(aRemovedStart)
	, mRemovedEnd(aRemovedEnd)
	, mBefore(aBefore)
	, mAfter(aAfter)
{
	assert(mAddedStart <= mAddedEnd);
	assert(mRemovedStart <= mRemovedEnd);
}

void TextEditor::UndoRecord::Undo(TextEditor* aEditor)
{
	if (!mAdded.empty())
	{
		aEditor->DeleteRange(mAddedStart, mAddedEnd);
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 2);
	}

	if (!mRemoved.empty())
	{
		auto start = mRemovedStart;
		aEditor->InsertTextAt(start, mRemoved.c_str());
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 2);
	}

	aEditor->mState = mBefore;
	aEditor->EnsureCursorVisible();

}

void TextEditor::UndoRecord::Redo(TextEditor* aEditor)
{
	if (!mRemoved.empty())
	{
		aEditor->DeleteRange(mRemovedStart, mRemovedEnd);
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 1);
	}

	if (!mAdded.empty())
	{
		auto start = mAddedStart;
		aEditor->InsertTextAt(start, mAdded.c_str());
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 1);
	}

	aEditor->mState = mAfter;
	aEditor->EnsureCursorVisible();
}

static bool TokenizeCStyleString(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;

	if (*p == '"')
	{
		p++;

		while (p < in_end)
		{
			// handle end of string
			if (*p == '"')
			{
				out_begin = in_begin;
				out_end = p + 1;
				return true;
			}

			// handle escape character for "
			if (*p == '\\' && p + 1 < in_end && p[1] == '"')
				p++;

			p++;
		}
	}

	return false;
}

static bool TokenizeCStyleCharacterLiteral(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;

	if (*p == '\'')
	{
		p++;

		while (p < in_end)
		{
			// handle end of string
			if (*p == '\'')
			{
				out_begin = in_begin;
				out_end = p + 1;
				return true;
			}

			// handle escape character for "
			if (*p == '\\' && p + 1 < in_end && p[1] == '\'')
				p++;

			p++;
		}
	}

	return false;
}

static bool TokenizeCStyleIdentifier(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;

	if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')
	{
		p++;

		while ((p < in_end) && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
			p++;

		out_begin = in_begin;
		out_end = p;
		return true;
	}

	return false;
}

static bool TokenizeCStyleNumber(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;

	const bool startsWithNumber = *p >= '0' && *p <= '9';

	if (*p != '+' && *p != '-' && !startsWithNumber)
		return false;

	p++;

	bool hasNumber = startsWithNumber;

	while (p < in_end && (*p >= '0' && *p <= '9'))
	{
		hasNumber = true;

		p++;
	}

	if (hasNumber == false)
		return false;

	bool isFloat = false;
	bool isHex = false;
	bool isBinary = false;

	if (p < in_end)
	{
		if (*p == '.')
		{
			isFloat = true;

			p++;

			while (p < in_end && (*p >= '0' && *p <= '9'))
				p++;
		}
		else if (*p == 'x' || *p == 'X')
		{
			// hex formatted integer of the type 0xef80

			isHex = true;

			p++;

			while (p < in_end && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
				p++;
		}
		else if (*p == 'b' || *p == 'B')
		{
			// binary formatted integer of the type 0b01011101

			isBinary = true;

			p++;

			while (p < in_end && (*p >= '0' && *p <= '1'))
				p++;
		}
	}

	if (isHex == false && isBinary == false)
	{
		// floating point exponent
		if (p < in_end && (*p == 'e' || *p == 'E'))
		{
			isFloat = true;

			p++;

			if (p < in_end && (*p == '+' || *p == '-'))
				p++;

			bool hasDigits = false;

			while (p < in_end && (*p >= '0' && *p <= '9'))
			{
				hasDigits = true;

				p++;
			}

			if (hasDigits == false)
				return false;
		}

		// single precision floating point type
		if (p < in_end && *p == 'f')
			p++;
	}

	if (isFloat == false)
	{
		// integer size type
		while (p < in_end && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L'))
			p++;
	}

	out_begin = in_begin;
	out_end = p;
	return true;
}

static bool TokenizeCStylePunctuation(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	(void)in_end;

	switch (*in_begin)
	{
	case '[':
	case ']':
	case '{':
	case '}':
	case '!':
	case '%':
	case '^':
	case '&':
	case '*':
	case '(':
	case ')':
	case '-':
	case '+':
	case '=':
	case '~':
	case '|':
	case '<':
	case '>':
	case '?':
	case ':':
	case '/':
	case ';':
	case ',':
	case '.':
		out_begin = in_begin;
		out_end = in_begin + 1;
		return true;
	}

	return false;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::CPlusPlus()
{
	static bool inited = false;
	static LanguageDefinition langDef;
	if (!inited)
	{
		static const char* const cppKeywords[] = {
			"and", "break", "do", "continue", "else", "elseif", "end", "false", "for", "function", "if", "in", "", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "goto",
			"TEXT_ALIGN_LEFT", "TEXT_ALIGN_CENTER", "TEXT_ALIGN_RIGHT", "TEXT_ALIGN_TOP", "TEXT_ALIGN_BOTTOM", "ESP_LEFT", "ESP_RIGHT", "ESP_TOP", "ESP_BOTTOM"
		};
		for (auto& k : cppKeywords)
			langDef.mKeywords.insert(k);

		static const char* const identifiers[] = {
			"assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs", "loadfile", "load", "loadstring",  "next",  "pairs",  "pcall",  "print",  "rawequal",  "rawlen",  "rawget",  "rawset",
			"select",  "setmetatable",  "tonumber",  "tostring",  "type",  "xpcall",  "_G",  "_VERSION","arshift", "band", "bnot", "bor", "bxor", "btest", "extract", "lrotate", "lshift", "replace",
			"rrotate", "rshift", "create", "resume", "running", "status", "wrap", "yield", "isyieldable", "debug","getuservalue", "gethook", "getinfo", "getlocal", "getregistry", "getmetatable",
			"getupvalue", "upvaluejoin", "upvalueid", "setuservalue", "sethook", "setlocal", "setmetatable", "setupvalue", "traceback", "close", "flush", "input", "lines", "open", "output", "popen",
			"read", "tmpfile", "type", "write", "close", "flush", "lines", "read", "seek", "setvbuf", "write", "__gc", "__tostring", "abs", "acos", "asin", "atan", "ceil", "cos", "deg", "exp", "tointeger",
			"floor", "fmod", "ult", "log", "max", "min", "modf", "rad", "random", "randomseed", "sin", "sqrt", "string", "tan", "type", "atan2", "cosh", "sinh", "tanh",
			"pow", "frexp", "ldexp", "log10", "pi", "huge", "maxinteger", "mininteger", "loadlib", "searchpath", "seeall", "preload", "cpath", "path", "searchers", "loaded", "module", "require", "clock",
			"date", "difftime", "execute", "exit", "getenv", "remove", "rename", "setlocale", "time", "tmpname", "byte", "char", "dump", "find", "format", "gmatch", "gsub", "len", "lower", "match", "rep",
			"reverse", "sub", "upper", "pack", "packsize", "unpack", "concat", "maxn", "insert", "pack", "unpack", "remove", "move", "sort", "offset", "codepoint", "char", "len", "codes", "charpattern",
			"coroutine", "table", "io", "os", "string", "utf8", "bit32", "math", "debug", "package"
		};
		for (auto& k : identifiers)
		{
			Identifier id;
			id.mDeclaration = "Native Lua Function";
			langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
		}

		static const char* const gLua[] = {
			"GetVolume", "GetFileName", "GetLevel", "GetPos", "Play", "Set3DEnabled", "SetPos", "EnableLooping", "GetTime", "GetState", "Get3DCone", "FFT", "Is3D", "GetPlaybackRate", "GetTagsOGG", "IsValid",
			"GetBitsPerSample", "Set3DCone", "GetTagsID3", "IsLooping", "Get3DEnabled", "SetPan", "SetVolume", "IsOnline", "GetSamplingRate", "Get3DFadeDistance", "GetLength", "GetTagsVendor", "IsBlockStreamed",
			"GetAverageBitRate", "Set3DFadeDistance", "GetTagsMeta", "Pause", "SetTime", "Stop", "GetTagsHTTP", "SetPlaybackRate", "GetPan", "GetRangeTo", "GetSolidMask", "GetMaxVisionRange", "IsAbleToSee", "BodyMoveXY",
			"SetSolidMask", "__index", "GetFOV", "IsNextBot", "SetFOV", "SetMaxVisionRange", "__newindex", "BecomeRagdoll", "GetRangeSquaredTo", "GetActivity", "StartActivity", "GetGoalTolerance", "Invalidate", "FirstSegment",
			"Compute", "GetClosestPosition", "Update", "PriorSegment", "GetMinLookAheadDistance", "GetPositionOnPath", "GetCurrentGoal", "LastSegment", "MoveCursor", "GetCursorPosition", "MoveCursorToStart", "GetEnd",
			"MoveCursorToEnd", "ResetAge", "Chase", "GetCursorData", "GetAllSegments", "SetMinLookAheadDistance", "GetHindrance", "NextSegment", "GetAge", "GetStart", "MoveCursorToClosestPosition", "MoveCursorTo",
			"Draw", "SetGoalTolerance", "SetJumpHeight", "SetDesiredSpeed", "Jump", "GetDeceleration", "SetDeceleration", "GetCurrentAcceleration", "GetAcceleration", "SetDeathDropHeight", "SetAcceleration",
			"GetGroundMotionVector", "JumpAcrossGap", "SetMaxYawRate", "SetStepHeight", "ClearStuck", "GetStepHeight", "SetVelocity", "GetNextBot", "IsStuck", "GetMaxYawRate", "IsOnGround", "Approach", "IsUsingLadder",
			"IsAttemptingToMove", "SetGravity", "GetVelocity", "GetMaxJumpHeight", "IsAreaTraversable", "GetGravity", "IsClimbingOrJumping", "FaceTowards", "GetJumpHeight", "GetDeathDropHeight", "GetVolume", "SetDSP",
			"GetPitch", "ChangeVolume", "GetDSP", "FadeOut", "IsPlaying", "PlayEx", "GetSoundLevel", "ChangePitch", "SetSoundLevel", "GetOldAngles", "GetButtons", "GetAbsMoveAngles", "SetSideSpeed", "GetOrigin",
			"GetMoveAngles", "SetImpulseCommand", "SetUpSpeed", "KeyWasDown", "SetAngles", "SetMaxSpeed", "SetButtons", "SetAbsMoveAngles", "GetMaxSpeed", "GetImpulseCommand", "SetConstraintRadius", "GetOldButtons",
			"GetAngles", "SetMaxClientSpeed", "SetOldButtons", "GetUpSpeed", "AddKey", "GetForwardSpeed", "KeyReleased", "KeyPressed", "KeyDown", "SetOrigin", "GetSideSpeed", "GetMaxClientSpeed", "SetForwardSpeed",
			"SetMoveAngles", "GetConstraintRadius", "SetOldAngles", "Finish", "SetRecordSound", "AddFrame", "Width", "Height", "SetNoDraw", "SetBBox", "SetParticleCullRadius", "Add", "GetNumActiveParticles",
			"SetNearClip", "CursorPos", "SlideDown", "IsVisible", "LoadGWENFile", "DragMousePress", "IsLoading", "ToggleSelection", "ParentToHUD", "GetDock", "SetPaintBorderEnabled", "SetTextInset", "KillFocus",
			"SetWidth", "GotoTextEnd", "SetKeyboardInputEnabled", "SizeToChildren", "SetMinimumSize", "SetTooltipPanel", "HasChildren", "DrawTextEntryText", "IsWorldClicker", "Refresh", "Remove", "IsMarkedForDeletion",
			"GetDockPadding", "GetCookie", "InvalidateParent", "GetBounds", "SetSize", "MoveAbove", "SetSkin", "GetZPos", "SaveUndoState", "LocalCursorPos", "GetFGColor", "SetSteamID", "InsertClickableTextEnd", "CopyPos",
			"GetTooltipPanel", "SetAchievement", "SetSelectionCanvas", "CopyBounds", "SetFontInternal", "IsMouseInputEnabled", "Center", "SetFGColor", "SetExpensiveShadow", "SetAutoDelete", "CenterHorizontal",
			"SetActionFunction", "Undo", "SetName", "LerpPositions", "RebuildSpawnIconEx", "SetToolTip", "SetToolTipPanel", "IsChildHovered", "SetDrawLanguageIDAtLeft", "UnselectAll", "DragClick", "SetAlpha",
			"GetCookieName", "Prepare", "SetDrawLanguageID", "StretchRightTo", "DockPadding", "SetDrawOnTop", "GWEN_SetMin", "GetFont", "SetVerticalScrollbarEnabled", "DoModal", "GetMaximumCharCount", "PostMessage",
			"SetCursor", "SetMouseInputEnabled", "NewObject", "SizeTo", "Queue", "DragHoverClick", "SetHTML", "InsertClickableTextStart", "Droppable", "DragHover", "InsertColorChange", "DrawOutlinedRect",
			"SetToFullHeight", "SelectAll", "GetName", "AnimTail", "MoveToBack", "PositionLabel", "GWEN_SetCheckboxText", "SetParent", "AnimationThinkInternal", "DisableLerp", "GetClosestChild", "MoveRightOf",
			"HasParent", "DragHoverEnd", "SetPaintBackgroundEnabled", "GetHTMLMaterial", "SetDragParent", "SetTerm", "MouseCapture", "GetChildren", "GetSelectionCanvas", "CenterVertical", "InvalidateChildren",
			"GetNumLines", "AlignLeft", "GetBGColor", "MakePopup", "IsSelected", "GetClassName", "AlignTop", "SetURL", "LoadControlsFromString", "Dock", "SizeToContents", "SetBGColor", "ColorTo",
			"EndBoxSelection", "AddText", "SetTall", "SetSpawnIcon", "CopySelected", "DragMouseRelease", "SetTooltip", "GotoTextStart", "SlideUp", "GetTooltip", "SetTabPosition", "LocalToScreen", "PaintAt",
			"DroppedOn", "IsSelectable", "SetDropTarget", "IsDragging", "IsKeyboardInputEnabled", "SetAnimationEnabled", "GoToHistoryOffset", "InsertFade", "ApplyGWEN", "GetText", "AppendText", "Valid",
			"GetTextInset", "GWEN_SetDock", "GWEN_SetHorizontalAlign", "GWEN_SetMax", "GWEN_SetMargin", "GWEN_SetControlName", "MoveBy", "GetValue", "LoadControlsFromFile", "GWEN_SetText", "GWEN_SetSize",
			"GWEN_SetPosition", "LoadGWENString", "GetSelectedChildren", "OpenURL", "Hide", "CopyWidth", "RequestFocus", "GoBack", "LoadTGAImage", "Show", "GetTextSize", "OnStopDragging", "IsHovered", "Clear",
			"StretchBottomTo", "NewObjectCallback", "MoveToBefore", "MoveToAfter", "NumSelectedChildren", "RebuildSpawnIcon", "CopyBase", "IsOurChild", "GetChildPosition", "SetAllowNonAsciiCharacters",
			"DistanceFrom", "__eq", "Distance", "IsSelectionCanvas", "CutSelected", "GetSkin", "SetTooltipPanelOverride", "SizeToContentsX", "MoveLeftOf", "SetPlayer", "SetFocusTopLevel", "DockMargin",
			"SizeToContentsY", "MoveBelow", "GetCookieNumber", "GetSize", "AlphaTo", "SetUnderlineFont", "ToggleVisible", "MoveToFront", "GetAlpha", "GetCaretPos", "SetPaintFunction", "SetSelected",
			"SetModel", "GetValidReceiverSlot", "GetChild", "StartBoxSelection", "HasHierarchicalFocus", "IsModal", "IsEnabled", "IsMultiline", "SetMultiline", "DrawTexturedRect", "FocusNext",
			"GetSelectedTextRange", "ScreenToLocal", "SetText", "SetVisible", "SelectAllOnFocus", "SelectNone", "SetContentAlignment", "SetKeyBoardInputEnabled", "ChildrenSize", "DeleteCookie", "SetZPos",
			"SetWorldClicker", "GoForward", "GetTable", "GetParent", "RunJavascript", "SetSelectable", "GetContentSize", "AlignBottom", "UpdateHTMLTexture", "ResetAllFades", "Command", "PaintManual", "DrawFilledRect",
			"Paste", "SetFGColorEx", "SelectAllText", "DrawDragHover", "SetWrap", "SetMaximumCharCount", "Find", "HasFocus", "IsPopup", "SetPopupStayAtBack", "DrawSelections", "SetEnabled", "FocusPrevious",
			"MoveTo", "SetRenderInScreenshots", "IsDraggable", "OnDrop", "GetWide", "OnStartDragging", "AlignRight", "SetPaintedManually", "NoClipping", "GetDockMargin", "SetCookie", "GetChildrenInRect", "Receiver",
			"SetCookieName", "NewAnimation", "StopLoading", "SetCaretPos", "GetTall", "SetBGColorEx", "SetHeight", "SetWide", "StretchToParent", "InvalidateLayout", "CopyHeight", "Exec", "ChildCount",
			"SetCommand", "Init", "Paint", "GenerateExample", "SetExpanded", "GetExpanded", "Setup", "GetDecimals", "SetConVarG", "GetColor", "DoConVarThink", "GetConVarR", "ConVarThink", "GetPalette",
			"Think", "SetLabel", "PerformLayout", "SetConVarA", "GetVector", "SetConVarR", "GetConVarG", "GetConVarB", "ValueChanged", "SetWangs", "SetColor", "UpdateConVars", "UpdateColor", "SetConVarB",
			"SetBaseColor", "SetVector", "SetAlphaBar", "TranslateValues", "GetWangs", "SetPalette", "GetConVarA", "UpdateConVar", "GetAlphaBar", "GetCanvas", "CreateRow", "GetCategory", "AllowInput",
			"OnKeyCodeTyped", "OnMousePressed", "OpenAutoComplete", "SetDrawBackground", "GetInt", "GetNumeric", "IsEditing", "GetEnterAllowed", "OnGetFocus", "ConVarNumberThink", "OnRemove",
			"ApplySchemeSettings", "SetPlaceholderColor", "OnValueChange", "GetPlaceholderColor", "AddHistory", "OnKeyCode", "SetCursorColor", "OnLoseFocus", "GetFloat", "OnEnter", "UpdateConvarValue",
			"GetHighlightColor", "GetDisabled", "SetFont", "SetTabbingDisabled", "GetDrawBorder", "SetHighlightColor", "OnChange", "ConVarStringThink", "SetNumeric", "SetConVar", "GetTabbingDisabled",
			"UpdateFromMenu", "SetDisabled", "GetHistoryEnabled", "SetValue", "GetPaintBackground", "GetTextColor", "SetPaintBackground", "GetDrawBackground", "ConVarChanged", "SetDrawBorder",
			"SetPlaceholderText", "OnTextChanged", "GetCursorColor", "CheckNumeric", "SetTextColor", "SetHistoryEnabled", "UpdateFromHistory", "SetUpdateOnType", "GetPlaceholderText", "SetEnterAllowed",
			"GetUpdateOnType", "SetEditable", "GetAutoComplete", "EditVariable", "OnEntityLost", "RebuildControls", "PreAutoRefresh", "PostAutoRefresh", "EntityLost", "SetEntity", "GetPadding", "GetList",
			"DoExpansion", "SetPadding", "GetStartHeight", "LoadCookies", "SetStartHeight", "SetAnimTime", "UpdateAltLines", "AnimSlide", "SetContents", "GetAnimTime", "GetHeaderHeight", "Toggle", "SetHeaderHeight",
			"SetList", "OnToggle", "SetBorderColor", "SetType", "GetBorderColor", "GetType", "SetBorder", "GetTile", "GetBorder", "GetSpaceY", "FitsInTile", "Copy", "CopyContents", "ConsumeTiles", "OnChildAdded",
			"GetMinHeight", "GetSpaceX", "SetSpaceY", "OnChildRemoved", "SetMinHeight", "LayoutTiles", "SetTile", "FindFreeTile", "GetBaseSize", "ClearTiles", "OnModified", "Layout", "SetBaseSize", "SetSpaceX",
			"GetStretchWidth", "GetLayoutDir", "SetLayoutDir", "LayoutIcons_LEFT", "LayoutIcons_TOP", "SetStretchHeight", "GetStretchHeight", "SetStretchWidth", "UpdateColours", "DoClick", "DoRightClick", "SetMaxWidth",
			"GetColumnID", "GetFixedWidth", "GetTextAlign", "SetMinWidth", "ResizeColumn", "GetMinWidth", "GetMaxWidth", "GetDescending", "SetFixedWidth", "SetTextAlign", "SetDescending", "SetColumnID", "GetSizable",
			"Close", "OnClose", "SetDraggable", "SetIsMenu", "GetIsMenu", "GetScreenLock", "OnMouseReleased", "SetTitle", "SetScreenLock", "GetDraggable", "SetBackgroundBlur", "SetDeleteOnClose", "GetPaintShadow",
			"GetBackgroundBlur", "ShowCloseButton", "GetDeleteOnClose", "SetIcon", "SetPaintShadow", "SetSizable", "GetTitle", "IsActive", "OnMouseWheeled", "OnVScroll", "ScrollToChild", "InnerWidth", "GetVBar", "PerformLayoutInternal", "SetCanvas", "AddItem", "Rebuild", "GetModels", "SetPath", "SetBaseFolder", "SetOpen", "SetFileTypes", "ShowFolder", "GetFolderNode", "OnRightClick", "SetSearch", "GetSearch", "SetCurrentFolder", "SetupTree", "SetupFiles", "GetCurrentFolder", "OnSelect", "GetBaseFolder", "GetFileTypes", "OnDoubleClick", "SortFiles", "GetOpen", "SetModels", "GetPath", "GetRGB", "GetHue", "GetBaseRGB", "SetRGB", "SetHue", "OnUserChanged", "SetBaseRGB", "PaintOver", "SetReadOnly", "MakeDroppable", "GetDnD", "SetUseLiveDrag", "UpdateDropTarget", "DropAction_Normal", "DropAction_Simple", "GetUseLiveDrag", "DropAction_Copy", "SetDnD", "GetReadOnly", "SetDropPos", "GetOpenSize", "SetOpenSize", "Open", "GetOpenTime", "SetOpenTime", "GetShowIcons", "SetShowIcons", "GetItems", "CloseTab", "CrossFade", "SetupCloseButton", "GetActiveTab", "SizeToContentWidth", "SetActiveTab", "SetFadeTime", "SwitchToName", "OnActiveTabChanged", "GetFadeTime", "AddSheet", "StartScene", "SetDirectionalLight", "GetModel", "PreDrawModel", "SetAmbientLight", "SetLookAng", "GetEntity", "DrawModel", "PostDrawModel", "GetLookAng", "GetAnimSpeed", "SetCamPos", "SetLookAt", "SetAnimated", "SetAnimSpeed", "LayoutEntity", "GetAmbientLight", "GetCamPos", "GetAnimated", "RunAnimation", "GetLookAt", "DoLoadMaterial", "SetMaterial", "Unloaded", "FixVertexLitMaterial", "SetOnViewMaterial", "GetMatName", "GetFailsafeMatName", "LoadMaterial", "SetImage", "GetImageColor", "SetKeepAspect", "GetMaterial", "GetImage", "SetMatName", "SetFailsafeMatName", "GetKeepAspect", "SetImageColor", "BuildControls", "OnValueChanged", "AddScroll", "SetScroll", "GetHideButtons", "OnCursorMoved", "BarScale", "Grip", "GetOffset", "SetHideButtons", "Value", "SetUp", "AnimateTo", "GetScroll", "GetBottomMin", "GetTopMax", "StartGrab", "SetDragging", "SetTop", "GetTop", "GetDividerHeight", "SetBottomMin", "GetTopMin", "SetDividerHeight", "GetHoldPos", "GetBottom", "GetTopHeight", "SetHoldPos", "SetMiddle", "SetBottom", "SetTopMin", "SetTopHeight", "GetDragging", "DoConstraints", "GetMiddle", "SetTopMax", "ShowIcons", "GetParentNode", "InternalDoClick", "InsertNode", "GetLineHeight", "PopulateChildren", "GetShowFiles", "SetHideExpander", "LeaveTree", "GetDrawLines", "GetLastChild", "PerformRootNodeLayout", "SetNeedsChildSearch", "GetDoubleClickToOpen", "GetForceShowExpander", "SetDoubleClickToOpen", "InstallDraggable", "CreateChildNodes", "SetWildCard", "SetParentNode", "AddPanel", "FilePopulateCallback", "MoveChildTo", "GetPathID", "GetFolder", "SetDirty", "GetNeedsPopulating", "ChildExpanded", "GetChildNodes", "SetFolder", "SetShowFiles", "SetLastChild", "GetRoot", "SetForceShowExpander", "ExpandRecurse", "SetNeedsPopulating", "DoChildrenOrder", "IsRootNode", "SetupCopy", "GetChildNodeCount", "SetRoot", "PopulateChildrenAndSelf", "CleanList", "AddFolder", "GetIcon", "FilePopulate", "MakeFolder", "AddNode", "GetIndentSize", "SetDrawLines", "SetFileName", "Insert", "GetNeedsChildSearch", "GetHideExpander", "ExpandTo", "InternalDoRightClick", "SetPathID", "GetDraggableName", "OnNodeAdded", "SetDraggableName", "GetDirty", "GetChildNode", "GetWildCard", "OnNodeSelected", "MoveToTop", "DrawArrow", "PositionTooltip", "OpenForPanel", "GetRotation", "SetHandle", "GetHandle", "SetRotation", "SetSlideY", "GetSlideY", "SetSlideX", "GetSlideX", "SetBackground", "GetLockX", "GetTrapInside", "SetTrapInside", "GetLockY", "SetNotches", "SetNumSlider", "SetLockX", "GetNotches", "GetNumSlider", "SetLockY", "GetID", "GetSelected", "IsDown", "SetID", "GetPropertySheet", "GetPanel", "SetPropertySheet", "GetTabHeight", "SetPanel", "SetFraction", "GetFraction", "OnActivePanelChanged", "FindBestActive", "SelectPanel", "PaintInnerCorners", "PaintDifferentColours", "ResetToDefaultValue", "GetMax", "SetMinMax", "UpdateNotches", "SetMax", "SetMin", "GetDefaultValue", "GetMin", "GetRange", "TranslateSliderValues", "SetDecimals", "GetTextArea", "SetDark", "SetDefaultValue", "SetButtonSize", "SetSelectedNumber", "SetStickyKeys", "GetStickyKeys", "ButtonPressed", "GetButtonSize", "GetSelectedNumber", "OnButtonPressed", "SetLife", "GetSpacing", "SetAlignment", "Shuffle", "GetAlignment", "GetLife", "SetSpacing", "AddModelList", "SetModelList", "DoClickInternal", "GetChecked", "SetIsCheckable", "ToggleCheck", "GetMenu", "SetMenu", "OnCursorEntered", "GetIsCheckable", "AddSubMenu", "OnCursorExited", "SetSubMenu", "SetChecked", "OnChecked", "GetOpenMenu", "AddOrGetMenu", "AddMenu", "AddSpacer", "SetMaxHeight", "OptionSelected", "AddOption", "GetMaxHeight", "SetDeleteSelf", "CloseSubMenu", "GetDrawColumn", "SetMinimumWidth", "GetDeleteSelf", "HighlightItem", "OptionSelectedInternal", "ClearHighlights", "SetOpenSubMenu", "SetDrawColumn", "GetOpenSubMenu", "AddCVar", "OpenSubMenu", "GetMinimumWidth", "GetAltLine", "GetColumnText", "GetSortValue", "GetListView", "SetColumnText", "DataLayout", "SetAltLine", "IsLineSelected", "SetSortValue", "SetListView", "SortByColumn", "SelectItem", "FixColumnsLayout", "OnRowSelected", "SetHideHeaders", "DisableScrollbar", "SetSortable", "GetLines", "SetMultiSelect", "OnRequestResize", "GetMultiSelect", "GetSortedID", "OnScrollbarAppear", "GetSortable", "AddLine", "OnClickLine", "GetSelectedLine", "OnRowRightClick", "DoDoubleClick", "SelectFirstItem", "AddColumn", "ColumnWidth", "RemoveLine", "GetHideHeaders", "GetInnerTall", "GetLine", "SetDataHeight", "SortByColumns", "GetDataHeight", "ClearSelection", "GetMother", "SetMother", "Select", "GetAutoStretch", "OnLabelTextChanged", "SetAutoStretch", "SetDoubleClickingEnabled", "SetAutoStretchVertical", "SetBright", "GetHighlight", "OnDepressed", "DoDoubleClickInternal", "UpdateFGColor", "GetBright", "DoMiddleClick", "GetIsToggle", "GetTextStyleColor", "OnToggled", "OnReleased", "SetToggle", "SetTextStyleColor", "GetDoubleClickingEnabled", "SetIsToggle", "GetToggle", "GetDark", "GetAutoStretchVertical", "SetHighlight", "GetStretchToFit", "SetDepressImage", "SetImageVisible", "DepressImage", "GetDepressImage", "SetStretchToFit", "SetSelectedIcon", "SetManual", "FilterByText", "Fill", "GetSelectedIcon", "GetManual", "OnChangeInternal", "ScrollToSelected", "SelectIcon", "FinishedLoading", "UpdateHistory", "SetButtonColor", "StartedLoading", "UpdateNavButtonStatus", "OnFinishLoadingDocument", "OnCallback", "OnDocumentReady", "ConsoleMessage", "OnChangeTitle", "SetAllowLua", "GetScrollbars", "QueueJavascript", "SetScrollbars", "Call", "GetAllowLua", "OnChangeTargetURL", "AddFunction", "OnChildViewCreated", "OnBeginLoadingDocument", "GetOverlap", "SetOverlap", "GetShowDropTargets", "OnDragModified", "SetShowDropTargets", "SetDividerWidth", "GetLeftWidth", "SetLeftMin", "GetRightMin", "SetRightMin", "GetLeft", "GetDividerWidth", "SetRight", "GetLeftMin", "SetLeft", "GetRight", "SetLeftWidth", "GetCols", "SortByMember", "SetRowHeight", "SetColWide", "GetRowHeight", "GetColWide", "RemoveItem", "SetCols", "ControlHelp", "ListBox", "NumSlider", "PanelSelect", "GetAutoSize", "Button", "CheckBox", "NumberWang", "SetAutoSize", "TextEntry", "Help", "ComboBox", "GetOptionTextByData", "GetSelectedID", "GetOptionData", "ChooseOption", "SetSortItems", "AddChoice", "IsMenuOpen", "CheckConVarChanges", "ChooseOptionID", "CloseMenu", "OpenMenu", "GetOptionText", "GetSortItems", "GetActiveButton", "UseButtonOnlyStyle", "SetActiveButton", "ResetSavedColors", "Reset", "NetworkColorChange", "GetNumRows", "SaveColor", "OnRightClickButton", "SetColorButtons", "SetNumRows", "SetConsoleCommand", "OpenForPos", "SetBackgroundColor", "GetBackgroundColor", "UpdateText", "CaptureMouse", "SetFirstPerson", "GetFirstPerson", "FirstPersonControls", "GetFloatValue", "SetInterval", "HideWang", "SetFloatValue", "GetInterval", "GetTextValue", "SetZoom", "GetActive", "IdealZoom", "GetShouldDrawScreen", "LockCursor", "PaintScratchWindow", "GetZoom", "SetShouldDrawScreen", "DrawScreen", "SetActive", "DrawNotches", "GetSizeX", "SetSizeY", "GetSizeY", "SetSizeX", "GetIndent", "SetIndent", "SetLineHeight", "SetIndentSize", "SetSelectedItem", "SetClickOnDragHover", "Root", "LayoutTree", "GetClickOnDragHover", "GetSelectedItem", "SetBarColor", "GetBarColor", "EnableVerticalScrollbar", "InsertAtTop", "EnableHorizontal", "InsertBefore", "DropAction", "GetNoSizing", "SetStretchHorizontally", "SetNoSizing", "GetAnimEase", "SetAnimEase", "InsertAfter", "GetStretchHorizontally", "GetPosColor", "SetValueOff", "GetValueOff", "GetConVar", "SetValueOn", "GetValueOn", "SetSelectedItems", "SelectByName", "GetSelectedItems", "GetSelectedValues", "SetMultiple", "GetMultiple", "SetRow", "GetRow", "SetCostSoFar", "IsCompletelyVisible", "IsOpen", "IsPotentiallyVisible", "IsCoplanar", "IsOverlapping", "GetPlace", "IsClosed", "IsFlat", "IsConnectedAtSide", "GetParentHow", "ComputeGroundHeightChange", "GetLadders", "GetClosestPointOnArea", "PopOpenList", "GetRandomPoint", "GetHidingSpots", "IsOpenListEmpty", "UpdateOnOpenList", "PlaceOnGround", "IsConnected", "SetCorner", "AddToClosedList", "SetTotalCost", "IsBlocked", "IsPartiallyVisible", "IsUnderwater", "GetExposedSpots", "RemoveFromClosedList", "SetAttributes", "GetRandomAdjacentAreaAtSide", "Contains", "GetAdjacentCount", "GetAdjacentAreasAtSide", "GetAdjacentCountAtSide", "GetZ", "GetAdjacentAreas", "AddHidingSpot", "AddToOpenList", "HasAttributes", "Disconnect", "GetCorner", "GetLaddersAtSide", "IsOverlappingArea", "ComputeAdjacentConnectionHeightChange", "GetExtentInfo", "ClearSearchLists", "GetAttributes", "ComputeDirection", "GetCostSoFar", "IsRoughlySquare", "ConnectTo", "SetPlace", "GetIncomingConnections", "DrawSpots", "GetTotalCost", "GetIncomingConnectionsAtSide", "GetCenter", "GetWheelContactPoint", "GetWheelTotalHeight", "GetPassenger", "SetWheelFriction", "IsEngineStarted", "GetPassengerSeatPoint", "BoostTimeLeft", "GetDriver", "IsEngineEnabled", "IsValidVehicle", "CheckExitPoint", "SetSteering", "GetWheel", "GetThirdPersonMode", "SetBoost", "SetCameraDistance", "GetWheelBaseHeight", "SetVehicleEntryAnim", "SetVehicleParams", "GetVehicleViewPosition", "HasBoost", "SetSteeringDegrees", "GetSteering", "GetHLSpeed", "GetWheelCount", "SetVehicleClass", "GetVehicleParams", "IsVehicleBodyInWater", "HasBrakePedal", "GetSteeringDegrees", "SetHasBrakePedal", "ReleaseHandbrake", "EnableEngine", "IsVehicle", "SetHandbrake", "GetSpeed", "SetMaxReverseThrottle", "GetRPM", "SetMaxThrottle", "GetThrottle", "SetThrottle", "IsBoosting", "GetCameraDistance", "StartEngine", "SetSpringLength", "GetVehicleClass", "GetOperatingParams", "SetThirdPersonMode", "GetAmmo", "GetDefault", "SetInt", "GetString", "SetFloat", "GetFlags", "Revert", "IsFlagSet", "SetString", "GetHelpText", "GetBool", "SetBool", "GetTopRightArea", "GetTopBehindArea", "SetTopForwardArea", "SetTopBehindArea", "GetTopForwardArea", "GetWidth", "SetTopRightArea", "SetTopLeftArea", "GetBottomArea", "GetTopLeftArea", "GetNormal", "GetPosAtHeight", "SetBottomArea", "SetUnpacked", "ToVector", "Unpack", "ToTable", "ToHSL", "ToHSV", "Invert", "SetScale", "Zero", "__mul", "__sub", "IsIdentity", "GetTranslation", "GetInverseTR", "Rotate", "SetField", "ScaleTranslation", "GetInverse", "SetForward", "GetScale", "GetForward", "Identity", "Set", "GetField", "Translate", "SetTranslation", "Scale", "InvertTR", "IsRotationMatrix", "__add", "IsZero", "GetUp", "SnapTo", "Right", "RotateAroundAxis", "Sub", "Forward", "__div", "__unm", "Up", "Mul", "Div", "Normalize", "TargetOrder", "Give", "SetSchedule", "IsNPC", "AlertSound", "ClearSchedule", "SetIdealActivity", "IsSquadLeader", "MaintainActivity", "SelectWeapon", "CapabilitiesRemove", "HasEnemyMemory", "SetNPCState", "CapabilitiesAdd", "SetArrivalSpeed", "ClearEnemyMemory", "SetIdealYawAndUpdate", "SetHullSizeNormal", "GetActiveWeapon", "NavSetRandomGoal", "SetArrivalDirection", "SetMaxRouteRebuildTime", "NavSetGoal", "RunEngineTask", "GetExpression", "UseNoBehavior", "MoveOrder", "StopMoving", "GetShootPos", "AddEntityRelationship", "SetSquad", "ClearExpression", "GetSquad", "RememberUnreachable", "FearSound", "IsUnreachable", "GetArrivalSequence", "UseFuncTankBehavior", "SetHullType", "SetArrivalSequence", "UpdateEnemyMemory", "Disposition", "GetCurrentWeaponProficiency", "UseAssaultBehavior", "UseLeadBehavior", "TaskComplete", "SetCurrentWeaponProficiency", "SetEnemy", "SetLastPosition", "GetHullType", "UseActBusyBehavior", "IsRunningBehavior", "ClearCondition", "GetAimVector", "GetNearestSquadMember", "SetArrivalDistance", "GetMovementSequence", "GetPathDistanceToGoal", "GetWeapon", "GetEnemyLastKnownPos", "GetWeapons", "PickupWeapon", "DropWeapon", "GetNextWaypointPos", "GetCurWaypointPos", "AddRelationship", "GetEnemy", "ClearGoal", "Classify", "GetEnemyFirstTimeSeen", "GetPathTimeToGoal", "SetArrivalActivity", "GetEnemyLastTimeSeen", "SetMovementSequence", "NavSetGoalTarget", "GetTarget", "SetMovementActivity", "GetMovementActivity", "TaskFail", "HasEnemyEluded", "GetNPCState", "LostEnemySound", "CapabilitiesGet", "MarkEnemyAsEluded", "SetCondition", "HasCondition", "GetIdealActivity", "StartEngineTask", "SetTarget", "GetArrivalActivity", "GetCurrentSchedule", "IdleSound", "NavSetWanderGoal", "ExitScriptedSequence", "GetBlockingEntity", "SetExpression", "IsMoving", "UseFollowBehavior", "PlaySentence", "CapabilitiesClear", "GetEnemyLastSeenPos", "FoundEnemySound", "SentenceStop", "ConditionName", "IsCurrentSchedule", "StopWalking", "GiveAmmo", "SuppressHint", "SendHint", "GetCount", "AllowImmediateDecalPainting", "Frags", "AddCleanup", "SetTeam", "AddFrags", "LimitHit", "IsTimingOut", "AddCount", "Ban", "GetViewModel", "EquipSuit", "SetSuitPower", "SetHoveredWidget", "CheckLimit", "StartSprinting", "AddDeaths", "GetEyeTrace", "SetNoTarget", "GetInfo", "UniqueIDTable", "AnimSetGestureSequence", "GetPreferredCarryAngles", "AnimSetGestureWeight", "LagCompensation", "Team", "PhysgunUnfreeze", "AddFrozenPhysicsObject", "StripAmmo", "RemoveAmmo", "Alive", "SimulateGravGunPickup", "DrawViewModel", "GetVehicle", "IsUserGroup", "SetStepSize", "IsSuperAdmin", "SetEyeAngles", "IsWorldClicking", "IsAdmin", "SetUnDuckSpeed", "GetHoveredWidget", "SetViewEntity", "SetupHands", "PacketLoss", "CanUseFlashlight", "LastHitGroup", "SetActiveWeapon", "SwitchToDefaultWeapon", "RemovePData", "ViewPunchReset", "GetCurrentCommand", "GetPunchAngle", "Freeze", "GetStepSize", "SetPData", "GetAvoidPlayers", "SetArmor", "GetNoCollideWithTeammates", "SetLastHitGroup", "GetViewOffsetDucked", "Flashlight", "InVehicle", "SetAllowFullRotation", "Ping", "GetLaggedMovementValue", "GetAllowFullRotation", "SetAvoidPlayers", "GetPData", "IsFrozen", "GetObserverMode", "Name", "TimeConnected", "SprintDisable", "TraceHullAttack", "EnterVehicle", "DrawWorldModel", "GetTool", "SetCrouchedWalkSpeed", "DebugInfo", "StripWeapons", "SetLaggedMovementValue", "RemoveAllAmmo", "Kick", "Spectate", "TranslateWeaponActivity", "DropObject", "Lock", "KillSilent", "SetMaxArmor", "GetSuitPower", "PlayStepSound", "SetSuppressPickupNotices", "KeyDownLast", "GetEntityInUse", "GetObserverTarget", "IsSprinting", "SimulateGravGunDrop", "Say", "GetWeaponColor", "MotionSensorPos", "SetViewOffset", "SetHands", "GetHands", "SetViewPunchAngles", "ScreenFade", "GetAllowWeaponsInVehicle", "IsListenServerHost", "CrosshairDisable", "GetCanWalk", "UniqueID", "DoAttackEvent", "GetClassID", "SetRunSpeed", "IsTyping", "IsPlayingTaunt", "SetAmmo", "AccountID", "FlashlightIsOn", "ShouldDropWeapon", "GetTimeoutSeconds", "SetCurrentViewOffset", "SetViewOffsetDucked", "GetViewOffset", "GetSlowWalkSpeed", "SetAllowWeaponsInVehicle", "Crouching", "IPAddress", "SetObserverMode", "GodDisable", "UnSpectate", "DropNamedWeapon", "SetJumpPower", "SetWeaponColor", "DoCustomAnimEvent", "CrosshairEnable", "GetRagdollEntity", "GetDrivingEntity", "GetCanZoom", "SteamID64", "CreateRagdoll", "IsSuitEquipped", "SetClassID", "SetNoCollideWithTeammates", "ExitVehicle", "GetDrivingMode", "SetHullDuck", "SetCanWalk", "UnLock", "SprayDecal", "SetHull", "SetSlowWalkSpeed", "SetLadderClimbSpeed", "ChatPrint", "SetWalkSpeed", "SteamID", "GetHullDuck", "UserID", "GetViewPunchVelocity", "GetEyeTraceNoCursor", "StartWalking", "SetUserGroup", "SprintEnable", "GetUseEntity", "GetPressedWidget", "HasGodMode", "GetViewPunchAngles", "SetPressedWidget", "StopZooming", "PickupObject", "GetViewEntity", "GetLadderClimbSpeed", "SetViewPunchVelocity", "GetDuckSpeed", "SetDuckSpeed", "GetCrouchedWalkSpeed", "SetFrags", "GetRunSpeed", "IsBot", "SetPlayerColor", "GetWalkSpeed", "StopSprinting", "GetRenderAngles", "SetRenderAngles", "GetMaxArmor", "AnimRestartMainSequence", "AnimResetGestureSlot", "UnfreezePhysicsObjects", "HasWeapon", "AddVCDSequenceToGestureSlot", "RemoveSuit", "GetPlayerColor", "DoReloadEvent", "DoSecondaryAttack", "SpectateEntity", "OwnerSteamID64", "DoAnimationEvent", "IsDrivingEntity", "IsPlayer", "Deaths", "SetDrivingEntity", "GetJumpPower", "Armor", "SendLua", "GetInfoNum", "Nick", "GetAmmoCount", "GetPreviousWeapon", "GetCurrentViewOffset", "PrintMessage", "ResetHull", "IsFullyAuthenticated", "GetUserGroup", "DetonateTripmines", "StripWeapon", "AnimRestartGesture", "GodEnable", "GetUnDuckSpeed", "Kill", "AllowFlashlight", "ViewPunch", "SetCanZoom", "SetDeaths", "ConCommand", "GetHull", "RemoveAllItems", "VoiceVolume", "IsMuted", "AddPlayerOption", "SetMuted", "GetFriendStatus", "IsSpeaking", "ShouldDrawLocalPlayer", "IsVoiceAudible", "ShowProfile", "GetPlayerInfo", "GetNormalized", "DistToSqr", "Length2D", "Angle", "DotProduct", "IsEqualTol", "Length", "Length2DSqr", "WithinAABox", "ToColor", "Cross", "Dot", "LengthSqr", "AngleEx", "ToScreen", "ClearMovement", "SetMouseWheel", "ClearButtons", "GetImpulse", "SetMouseX", "TickCount", "GetMouseX", "SetImpulse", "RemoveKey", "SetSideMove", "GetViewAngles", "SetMouseY", "GetSideMove", "GetMouseWheel", "GetMouseY", "IsForced", "CommandNumber", "SetForwardMove", "SetUpMove", "GetForwardMove", "GetUpMove", "SetViewAngles", "ReadVector", "ReadFloat", "StartBlock", "ReadInt", "EndBlock", "ReadString", "ReadEntity", "ReadAngle", "ReadBool", "IsExplosionDamage", "SetDamageBonus", "IsDamageType", "SetDamageCustom", "IsFallDamage", "SetDamage", "GetInflictor", "GetBaseDamage", "SetDamageType", "SetBaseDamage", "SetDamagePosition", "GetMaxDamage", "GetAttacker", "GetDamage", "ScaleDamage", "GetDamageCustom", "SetReportedPosition", "SetInflictor", "SubtractDamage", "GetDamageForce", "SetMaxDamage", "SetAttacker", "GetDamageBonus", "GetDamageType", "AddDamage", "GetAmmoType", "SetAmmoType", "GetReportedPosition", "GetDamagePosition", "SetDamageForce", "IsBulletDamage", "ReadDouble", "ReadShort", "ReadUShort", "WriteFloat", "WriteByte", "ReadULong", "WriteLong", "WriteULong", "WriteDouble", "Skip", "Seek", "Size", "Tell", "WriteBool", "Flush", "ReadLine", "WriteUShort", "ReadLong", "Read", "WriteShort", "EndOfFile", "ReadByte", "Write", "GetOrthographic", "GetVerticalFOV", "SetTexture", "SetBrightness", "SetFarZ", "GetEnableShadows", "SetVerticalFOV", "GetConstantAttenuation", "GetHorizontalFOV", "GetTextureFrame", "SetTextureFrame", "GetQuadraticAttenuation", "SetEnableShadows", "SetHorizontalFOV", "GetFarZ", "GetBrightness", "GetLinearAttenuation", "SetNearZ", "SetLinearAttenuation", "SetConstantAttenuation", "SetQuadraticAttenuation", "SetOrthographic", "GetTexture", "GetNearZ", "GetWeight", "GetHoldType", "GetSlotPos", "GetSlot", "GetWeaponViewModel", "GetNextSecondaryFire", "LastShootTime", "Clip2", "HasAmmo", "IsWeaponVisible", "SetNextPrimaryFire", "SetNextSecondaryFire", "IsWeapon", "GetPrimaryAmmoType", "AllowsAutoSwitchFrom", "IsScripted", "GetSecondaryAmmoType", "AllowsAutoSwitchTo", "GetWeaponWorldModel", "GetPrintName", "CallOnClient", "DefaultReload", "Clip1", "GetNextPrimaryFire", "GetMaxClip2", "SendWeaponAnim", "SetHoldType", "SetClip1", "GetMaxClip1", "SetClip2", "SetLastShootTime", "IsCarriedByLocalPlayer", "GetHeight", "AddPlayer", "AddRecipientsByTeam", "AddAllPlayers", "RemoveRecipientsNotOnTeam", "RemoveRecipientsByTeam", "GetPlayers", "RemovePAS", "AddPVS", "RemoveAllPlayers", "RemovePVS", "RemovePlayer", "AddPAS", "GetMappingWidth", "IsErrorTexture", "Download", "GetNumAnimationFrames", "IsError", "GetMappingHeight", "LookupPoseParameter", "SetPreventTransmit", "GetSpawnFlags", "GetParentWorldTransformMatrix", "IsWidget", "GetNWEntity", "SetNotSolid", "SetShouldServerRagdoll", "SetTransmitWithParent", "GetCallbacks", "SetNetworkedNumber", "GetNW2VarTable", "IsLagCompensated", "EnableConstraints", "GetVar", "GetManipulateBonePosition", "SetAnimation", "SetNW2VarProxy", "DestroyBoneFollowers", "SetCollisionBoundsWS", "HasBoneManipulations", "SetTable", "GetHitBoxBounds", "Spawn", "GetNumPoseParameters", "SetDTInt", "GetSequenceList", "AddLayeredSequence", "GibBreakClient", "GetClass", "GetModelContents", "GetLocalAngularVelocity", "GetNW2VarProxy", "SetTrigger", "SetNetworked2VarProxy", "ClearPoseParameters", "SetNetworkedEntity", "SetUnFreezable", "GetUnFreezable", "InstallDataTable", "SetCustomCollisionCheck", "GetMomentaryRotButtonPos", "GetLocalAngles", "SetDTFloat", "CollisionRulesChanged", "SetNW2Int", "CallOnRemove", "GetModelBounds", "SetVar", "AddCallback", "GetFlexBounds", "ManipulateBoneJiggle", "IsConstrained", "GetNetworkedString", "GetRagdollOwner", "SetShouldPlayPickupSound", "SetPhysicsAttacker", "SetSaveValue", "AddFlags", "GetCustomCollisionCheck", "ResetSequenceInfo", "GetShouldPlayPickupSound", "GibBreakServer", "OBBMins", "FireBullets", "UpdateBoneFollowers", "GetBoneController", "CreateBoneFollowers", "PassesFilter", "TakeDamageInfo", "GetSequenceInfo", "SetNW2Angle", "GetDTString", "TranslatePhysBoneToBone", "__concat", "PlayScene", "GetFlexScale", "GetDTEntity", "SetLocalAngularVelocity", "GetNetworkedVarTable", "SetNetworked2Entity", "GetWorkshopID", "GetConstrainedPhysObjects", "GetCreationID", "SelectWeightedSequenceSeeded", "GetNetworkedVector", "SetLayerPriority", "StartLoopingSound", "LookupSequence", "RemoveGesture", "ManipulateBoneAngles", "AddGestureSequence", "GetLayerPlaybackRate", "GetLightingOriginEntity", "GetModelScale", "SetNetworked2Float", "GetLayerCycle", "SetNetworkOrigin", "SetPersistent", "DispatchTraceAttack", "IsPlayingGesture", "GetPersistent", "RemoveInternalConstraint", "GetBoneCount", "RestartGesture", "SetModelScale", "PhysicsInitConvex", "FindTransitionSequence", "IsWorld", "SetDTString", "GetSolidFlags", "SetLagCompensated", "SetLightingOriginEntity", "GetKeyValues", "SetLocalVelocity", "RagdollSolve", "GetDTBool", "RagdollUpdatePhysics", "HasFlexManipulatior", "RagdollStopControlling", "ViewModelIndex", "GetAbsVelocity", "GetEFlags", "GetShouldServerRagdoll", "SetAttachment", "SetRagdollPos", "IsPlayerHolding", "GetAttachment", "Input", "GetSequence", "IsSequenceFinished", "GetBodygroupName", "GetHitboxBone", "PhysicsInitSphere", "SetMoveCollide", "SetSolidFlags", "SetAbsVelocity", "GetEffects", "GetChildBones", "GetBodygroup", "Use", "MapCreationID", "GetNetworked2Float", "RemoveEffects", "SetMoveParent", "GetBoneMatrix", "IsConstraint", "GetMaterialType", "Weapon_TranslateActivity", "HeadTarget", "BodyTarget", "GetBloodColor", "GetLayerWeight", "GetCollisionGroup", "SetWeaponModel", "SetBloodColor", "NearestPoint", "SetBoneMatrix", "CreatedByMap", "GetPoseParameterRange", "SetHealth", "GetDTInt", "GetBrushPlane", "TakePhysicsDamage", "GetNoDraw", "Activate", "GetInternalVariable", "TakeDamage", "Extinguish", "GetCollisionBounds", "SetSequence", "SetModelName", "DeleteOnRemove", "SetCollisionBounds", "SetPhysConstraintObjects", "SetNW2Entity", "SetNetworked2Bool", "PointAtEntity", "GetSpawnEffect", "Respawn", "PhysicsInitStatic", "WorldToLocalAngles", "DropToFloor", "GetNetworked2Vector", "SetNetworkedBool", "GetPhysicsAttacker", "GetNetworked2VarProxy", "SetNetworkedVector", "AddEffects", "SendViewModelMatchingSequence", "IsRagdoll", "TestPVS", "IsInWorld", "IsEffectActive", "LocalToWorldAngles", "GetSequenceActivity", "GetNetworked2VarTable", "GetNWVarTable", "SetEyeTarget", "GetWorldTransformMatrix", "SetNWInt", "GetNetworked2Int", "GetPoseParameterName", "GetFlexWeight", "GetNetworked2Var", "SetNetworkedVar", "SetNetworked2Var", "GetNW2Entity", "SetBodygroup", "SetLayerBlendOut", "GetNetworked2Entity", "ForcePlayerDrop", "Blocked", "EntIndex", "SetFlexWeight", "GetNetworked2Angle", "SetRenderMode", "GetAttachments", "SetDTEntity", "GetNW2Vector", "SetRagdollBuildFunction", "SetMaxHealth", "SetNetworked2Vector", "SetOwner", "GetNW2String", "GetModelRadius", "SetLayerDuration", "GetSequenceMovement", "IsLineOfSightClear", "SetNW2String", "GetNetworked2String", "WaterLevel", "BoneHasFlag", "GetDTAngle", "GetNW2Bool", "GetRenderMode", "GetRenderFX", "GetBodygroupCount", "SetNW2Bool", "LookupAttachment", "SetRenderFX", "GetNetworked2Bool", "GetOwner", "GetNW2Float", "PrecacheGibs", "GetNW2Int", "RemoveCallOnRemove", "GetNWAngle", "SetNW2Var", "GetManipulateBoneAngles", "SelectWeightedSequence", "NextThink", "SetNetworked2Int", "SetNWBool", "SetSpawnEffect", "OnGround", "GetAnimInfo", "SetMoveType", "SetNWEntity", "SetNWAngle", "GetBrushSurfaces", "GetMoveType", "SetNWVector", "GetHitboxSet", "SetNWFloat", "GetModelRenderBounds", "GetSolid", "GetNW2Var", "RemoveEFlags", "SetNWString", "GetConstrainedEntities", "GetSequenceName", "SetNetworkedFloat", "Health", "SetNetworkedInt", "SetNetworkedString", "PhysicsInit", "GetCreator", "GetNetworkedEntity", "GetNWBool", "IsOnFire", "SetPoseParameter", "AddToMotionController", "PhysicsInitShadow", "GetHitboxSetCount", "GetNetworkedBool", "SetCollisionGroup", "GetNWVector", "SetLayerLooping", "GetElasticity", "GetNWFloat", "GetNWInt", "BoneLength", "GetNetworkedInt", "GetNetworkedFloat", "GetNetworkedVar", "SetKeyValue", "GetHitBoxCount", "UseTriggerBounds", "SetNWVarProxy", "StopParticles", "OBBCenter", "SetNetworkedVarProxy", "IsValidLayer", "SetLayerBlendIn", "SetDTAngle", "DrawShadow", "GetLayerSequence", "SetSolid", "GetDTVector", "SetLayerCycle", "GetCreationTime", "AddEFlags", "GetPhysicsObjectNum", "GetLayerDuration", "SetLayerWeight", "SetLayerPlaybackRate", "SetFriction", "Visible", "GetTouchTrace", "GetDTFloat", "GetFlexIDByName", "RemoveFromMotionController", "GetPoseParameter", "GetBodyGroups", "RemoveSolidFlags", "EyePos", "IsSolid", "GetMaterials", "ManipulateBonePosition", "StopMotionController", "IsDormant", "PhysicsDestroy", "WorldSpaceCenter", "RemoveCallback", "SetCreator", "SetBodyGroups", "SetNetworked2Angle", "UseClientSideAnimation", "ResetSequence", "SetNW2Float", "GetSubModels", "FrameAdvance", "SetHitboxSet", "GetSequenceGroundSpeed", "SetBoneController", "ObjectCaps", "GetGroundSpeedVelocity", "DontDeleteOnRemove", "WorldToLocal", "TranslateBoneToPhysBone", "GetFriction", "SkinCount", "GetSequenceCount", "GetMoveCollide", "BoundingRadius", "GetFlexNum", "GetNW2Angle", "GetBoneContents", "SetUseType", "SetElasticity", "SetLocalPos", "GetSequenceMoveDist", "EnableCustomCollisions", "StopSound", "OBBMaxs", "GetManipulateBoneJiggle", "GetPhysicsObject", "SetDTVector", "SetGroundEntity", "AddGesture", "SetCycle", "ManipulateBoneScale", "GetHitBoxHitGroup", "FollowBone", "IsEFlagSet", "MakePhysicsObjectAShadow", "GetParentAttachment", "GetParentPhysNum", "GetNetworkOrigin", "EmitSound", "SetParentPhysNum", "VisibleVec", "Weapon_SetActivity", "GetSubMaterial", "SetNetworkedAngle", "StartMotionController", "GetCycle", "SetNetworked2String", "GetBrushPlaneCount", "GetBaseVelocity", "GetBonePosition", "WorldSpaceAABB", "GetNetworkedAngle", "GetSaveTable", "GetHitBoxGroupCount", "LookupBone", "GetGroundEntity", "GetPhysicsObjectCount", "PhysicsInitMultiConvex", "HasSpawnFlags", "GetBoneName", "GetFlexName", "GetLocalPos", "AddSolidFlags", "GetMaxHealth", "GetNumBodyGroups", "PhysicsInitBox", "GetTransmitWithParent", "SetLayerSequence", "EyeAngles", "LocalToWorld", "GetHitBoxBone", "GetSequenceMoveYaw", "SequenceDuration", "StopLoopingSound", "SetDTBool", "SetNW2Vector", "GetMoveParent", "Ignite", "PassesDamageFilter", "RemoveFlags", "GetNWString", "GetSequenceActivityName", "RemoveAllDecals", "GetRotatedAABB", "PhysicsFromMesh", "AlignAngles", "SetSubMaterial", "MuzzleFlash", "Fire", "GetBoneSurfaceProp", "PhysWake", "SetLocalAngles", "SetFlexScale", "RemoveAllGestures", "SetRagdollAng", "GetBoneParent", "FindBodygroupByName", "GetManipulateBoneScale", "SetLOD", "SetNetworkAngles", "MarkShadowAsDirty", "StopParticlesNamed", "StopParticleEmission", "InitializeAsClientEntity", "UpdateShadow", "BecomeRagdollOnClient", "DestroyShadow", "SnatchModelInstance", "SetLegacyTransform", "EnableMatrix", "SetNextClientThink", "CreateParticleEffect", "CreateShadow", "GetRenderGroup", "SetRenderClipPlaneEnabled", "SetRenderBoundsWS", "SetRenderBounds", "InvalidateBoneCache", "SetupBones", "SetBonePosition", "SetIK", "GetRenderOrigin", "GetModelPhysBoneCount", "SetRenderOrigin", "GetRenderBounds", "DisableMatrix", "SetRenderClipPlane", "StopAndDestroyParticles", "SetupPhonemeMappings", "GetNetworkAngles", "GetPredictable", "SetPredictable", "StopParticlesWithNameAndAttachment", "Render", "SetControlPointForwardVector", "GetAutoUpdateBBox", "SetControlPointOrientation", "StopEmissionAndDestroyImmediately", "IsFinished", "SetControlPoint", "GetEffectName", "StartEmission", "SetIsViewModelEffect", "SetControlPointRightVector", "AddControlPoint", "Restart", "SetShouldDraw", "SetControlPointEntity", "SetControlPointUpVector", "StopEmission", "GetHighestControlPoint", "SetControlPointParent", "IsViewModelEffect", "SetSortOrigin", "GetEndSize", "SetStartLength", "SetCollide", "GetAirResistance", "GetRoll", "GetBounce", "GetStartLength", "GetStartSize", "SetEndLength", "GetEndAlpha", "GetAngleVelocity", "SetLighting", "GetRollDelta", "SetStartSize", "SetEndAlpha", "SetStartAlpha", "SetAngleVelocity", "GetStartAlpha", "SetDieTime", "SetNextThink", "SetLifeTime", "SetBounce", "SetThinkFunction", "SetRoll", "SetCollideCallback", "SetVelocityScale", "GetDieTime", "SetAirResistance", "GetEndLength", "SetRollDelta", "GetLifeTime", "SetEndSize", "Destroy", "TraceBox", "ReadChar", "ReadVectorNormal", "SetHitBox", "SetStart", "SetMagnitude", "GetHitBox", "SetFlags", "GetMagnitude", "SetRadius", "GetRadius", "GetSurfaceProp", "GetMaterialIndex", "GetEntIndex", "SetEntIndex", "SetMaterialIndex", "SetNormal", "SetSurfaceProp", "IsWater", "IsNoDraw", "IsSky", "GetVertices", "WriteEntity", "WriteAngle", "WriteInt", "WriteString", "WriteVector", "GetShadowPos", "WorldToLocalVector", "SetBuoyancyRatio", "IsMoveable", "IsAsleep", "IsGravityEnabled", "GetVelocityAtPoint", "GetShadowAngles", "SetDragCoefficient", "GetMass", "EnableMotion", "SetAngleDragCoefficient", "IsCollisionEnabled", "GetAABB", "GetStress", "AddGameFlag", "OutputDebugInfo", "ApplyForceOffset", "GetFrictionSnapshot", "LocalToWorldVector", "AddVelocity", "GetInertia", "ClearGameFlag", "SetInertia", "GetDamping", "GetPositionMatrix", "RecheckCollisionFilter", "EnableDrag", "GetInvInertia", "GetMesh", "GetMeshConvexes", "GetSurfaceArea", "SetDamping", "HasGameFlag", "GetEnergy", "IsPenetrating", "GetInvMass", "GetSpeedDamping", "EnableCollisions", "EnableGravity", "AddAngleVelocity", "SetVelocityInstantaneous", "CalculateVelocityOffset", "GetRotDamping", "ApplyForceCenter", "ComputeShadowControl", "SetMass", "Wake", "IsDragEnabled", "IsMotionEnabled", "CalculateForceOffset", "ApplyTorqueCenter", "GetContents", "Sleep", "GetMassCenter", "BuildFromTriangles", "Recompute", "GetMatrix", "GetVector4D", "GetShader", "SetShader", "GetVectorLinear", "SetVector4D", "SetUndefined", "SetMatrix", "GAMEMODE", "CLIENT", "SERVER", "EFFECT", "GM", "SWEP", "TOOL", "ENT", "DIconLayout", "DMenuOptionCVar", "DPanelList", "DListView_Line", "DNumSlider", "DPanelOverlay", "DListView_ColumnPlain", "DDragBase", "DIconBrowser", "DHTML", "DCategoryList", "DImage", "DTextEntry", "DListView_DraggerBar", "DColorMixer", "DFrame", "DCheckBox", "DColorCombo", "DScrollBarGrip", "Slider", "DHorizontalDivider", "DSlider", "DForm", "DNumPad", "DListViewLine", "DPanelSelect", "DListView_Column", "DListViewHeaderLabel", "DDrawer", "DScrollPanel", "DListBoxItem", "DListView", "DForm", "DImageButton", "DListLayout", "DBinder", "DNotify", "DColorButton", "VoiceNotify", "DColorPalette", "DNumberScratch", "DImage", "DListViewLabel", "DProperty_Int", "DProperty_Entity", "DProperty_VectorColor", "DRGBPicker", "DProperty_Float", "DEntityProperties", "DProperty_Combo", "DProperty_Boolean", "DVScrollBar", "DHTMLControls", "DVerticalDivider", "DVerticalDividerBar", "DNumberWang", "DTree_Node_Button", "DLabelEditable", "DTree_Node", "DGrid", "DCheckBox", "DExpandButton", "DKillIcon", "DMenu", "DCategoryHeader", "DTooltip", "DTileLayout", "DModelSelect", "DSprite", "DSizeToContents", "DShape", "DModelSelectMulti", "DMenuOption", "DProperty_Generic", "DMenuBar", "DCheckBoxLabel", "DPropertySheet", "DTab", "DProperties", "DAlphaBar", "DProgress", "DModelPanel", "DFileBrowser", "DColorCube", "Button", "DListBox", "DAdjustableModelPanel", "DComboBox", "DColumnSheet", "DButton", "DHorizontalDividerBar", "DBubbleContainer", "DTree", "DLabelURL", "DHorizontalScroller", "derma", "halo", "gui", "hook", "usermessage", "undo", "scripted_ents", "gameevent", "file", "constraint", "game", "gamemode", "menubar", "motionsensor", "hammer", "navmesh", "gmod", "duplicator", "language", "ai_task", "ws_dupe", "effects", "resource", "draw", "umsg", "cookie", "achievements", "concommand", "chat", "drive", "cvars", "debugoverlay", "gmsave", "matproxy", "list", "markup", "team", "http", "cleanup", "ents", "baseclass", "saverestore", "video", "cam", "physenv", "search", "render", "bit", "net", "numpad", "notification", "weapons", "killicon", "mesh", "surface", "ws_save", "skin", "ai_schedule", "controlpanel", "spawnmenu", "properties", "player", "menu", "dragndrop", "steamworks", "vgui", "gwen", "util", "sound", "gamemode", "presets", "sql", "engine", "widgets", "system", "player_manager", "_g", "construct", "timer", "frame_blend", "ai", "jit", "ShowHelp", "ShowSpare1", "ShowSpare2", "OnEntityCreated", "PlayerAuthed", "TeamBased", "ThisClass", "CalcMainActivity", "EntityRemoved", "VehicleMove", "FinishMove", "FolderName", "PlayerLoadout", "Saved", "ShowTeam", "CreateEntityRagdoll", "PlayerCanHearPlayersVoice", "KeyRelease", "PhysgunPickup", "PlayerHurt", "EntityTakeDamage", "CanPlayerUnfreeze", "PlayerRequestTeam", "PlayerDroppedWeapon", "DoPlayerDeath", "PreGamemodeLoaded", "FindUseEntity", "Think", "GetFallDamage", "PlayerSetHandsModel", "PlayerPostThink", "UpdateAnimation", "OnViewModelChanged", "PlayerButtonDown", "PlayerDriveAnimate", "PlayerSwitchWeapon", "PlayerJoinTeam", "OnPhysgunFreeze", "OnDamagedByExplosion", "Tick", "SetupPlayerVisibility", "PlayerDeath", "OnPhysgunReload", "Initialize", "MouthMoveAnimation", "GravGunPickupAllowed", "GravGunPunt", "CanExitVehicle", "GravGunOnPickedUp", "PlayerInitialSpawn", "PlayerButtonUp", "ShutDown", "PlayerNoClip", "ShouldCollide", "StartEntityDriving", "GrabEarAnimation", "OnReloaded", "InitPostEntity", "PlayerShouldTakeDamage", "AllowPlayerPickup", "PlayerStartTaunt", "PlayerShouldTaunt", "KeyPress", "PlayerCanSeePlayersChat", "OnPlayerHitGround", "PlayerSpray", "CanPlayerEnterVehicle", "IsSpawnpointSuitable", "Website", "PlayerSilentDeath", "Email", "PlayerCanJoinTeam", "PlayerSwitchFlashlight", "PlayerLeaveVehicle", "PhysgunDrop", "PlayerFootstep", "Author", "Folder", "PlayerEnteredVehicle", "CanPlayerSuicide", "SetPlayerSpeed", "PlayerDeathSound", "PropBreak", "Name", "TranslateActivity", "ScalePlayerDamage", "WeaponEquip", "PlayerConnect", "PlayerSelectSpawn", "GetGameDescription", "PlayerSelectTeamSpawn", "SetupMove", "HandlePlayerSwimming", "PlayerStepSoundTime", "HandlePlayerNoClipping", "PlayerTick", "PlayerSpawn", "HandlePlayerJumping", "PlayerSpawnAsSpectator", "ScaleNPCDamage", "PlayerSay", "HandlePlayerDriving", "CanProperty", "EntityKeyValue", "VariableEdited", "PlayerDeathThink", "EndEntityDriving", "PlayerCanPickupWeapon", "PostGamemodeLoaded", "GravGunOnDropped", "OnNPCKilled", "HandlePlayerVaulting", "Restored", "CanEditVariable", "NetworkIDValidated", "HandlePlayerDucking", "PlayerTraceAttack", "CheckPassword", "CreateTeams", "HandlePlayerLanding", "PlayerSetModel", "OnPlayerChangedTeam", "PlayerUse", "DoAnimationEvent", "OnGamemodeLoaded", "CalcViewModelView", "PickupHistoryTop", "AdjustMouseSensitivity", "PlayerStartVoice", "StartChat", "FinishChat", "PreDrawHUD", "HUDShouldDraw", "HideTeam", "PostRender", "PreDrawTranslucentRenderables", "PreventScreenClicks", "PostDrawViewModel", "PreDrawViewModel", "NetworkEntityCreated", "OnSpawnMenuOpen", "HUDDrawTargetID", "PreRender", "PlayerBindPress", "HUDDrawPickupHistory", "PrePlayerDraw", "HUDDrawScoreBoard", "PostDrawTranslucentRenderables", "ChatTextChanged", "PostRenderVGUI", "OnSpawnMenuClose", "OnPlayerChat", "OnChatTab", "DrawPhysgunBeam", "PostDrawOpaqueRenderables", "CreateMove", "PreDrawOpaqueRenderables", "PreDrawHalos", "AddDeathNotice", "OnContextMenuOpen", "PlayerEndVoice", "HUDItemPickedUp", "PostPlayerDraw", "PostDrawEffects", "PreDrawEffects", "DrawMonitors", "ChatText", "GUIMousePressed", "PlayerClassChanged", "GUIMouseReleased", "PostDrawHUD", "PickupHistoryLast", "RenderScene", "DrawOverlay", "HUDPaintBackground", "CloseDermaMenus", "OnContextMenuClose", "InputMouseApply", "CreateClientsideRagdoll", "ShouldDrawLocalPlayer", "GetTeamNumColor", "DrawDeathNotice", "ScoreboardShow", "PickupHistoryCorner", "PreDrawSkyBox", "HUDPaint", "HUDWeaponPickedUp", "OnAchievementAchieved", "GUIMouseDoublePressed", "CalcView", "RenderScreenspaceEffects", "GetTeamColor", "PostProcessPermitted", "GetMotionBlurValues", "PostDraw2DSkyBox", "CalcVehicleView", "ForceDermaSkin", "HUDAmmoPickedUp", "PostDrawSkyBox", "PickupHistoryWide", "ScoreboardHide)", "((TOOL", "self)(\.", ":))(WorldModel", "SetNPCMaxRest", "PrintName", "Holster", "SetDeploySpeed", "CanSecondaryAttack", "EquipAmmo", "OnRemove", "Contact", "Reload", "SetupWeaponHoldTypeForAI", "NPCShoot_Secondary", "GetToolObject", "CanDeploy", "ShouldDropOnDie", "OnRestore", "SetupDataTables", "FireAnimationEvent", "ViewModelFlip", "ShootEffects", "CanHolster", "GetCapabilities", "AutoSwitchFrom", "SetNPCMinBurst", "CanPrimaryAttack", "Spawnable", "Precache", "AutoSwitchTo", "Instructions", "TakePrimaryAmmo", "AcceptInput", "Equip", "UseHands", "Folder", "TakeSecondaryAmmo", "SetNPCFireRate", "OwnerChanged", "PrimaryAttack", "ClassName", "GetNPCMinBurst", "GetNPCFireRate", "TranslateActivity", "Deploy", "ViewModelFOV", "Ammo2", "NPCShoot_Primary", "Initialize", "DoImpactEffect", "ShootBullet", "Ammo1", "KeyValue", "SetNPCMinRest", "CheckLimit", "GetMode", "Weight", "SetWeaponHoldType", "ShootSound", "GetNPCMaxRest", "ViewModel", "InitializeTools", "Base", "GetNPCMaxBurst", "Think", "DoShootEffect", "SecondaryAttack", "SetNPCMaxBurst", "AdminOnly", "Purpose", "OnDrop", "GetNPCMinRest", "Author", "StartGhostEntities", "AdjustMouseSensitivity", "WepSelectIcon", "ToolNameHeight", "SpeechBubbleLid", "CustomAmmoDisplay", "ClearObjects", "SetStage", "Gradient", "DrawWeaponInfoBox", "OnReloaded", "BobScale", "GetStage", "InfoBoxHeight", "RenderGroup", "DrawWorldModelTranslucent", "GetTracerOrigin", "DrawHUD", "DrawWeaponSelection", "BounceWeaponIcon", "DrawWorldModel", "TranslateFOV", "RenderScreen", "SlotPos", "ViewModelDrawn", "GetViewModelPosition", "InfoIcon", "PrintWeaponInfo", "FreezeMovement", "DrawAmmo", "SwayScale", "DrawCrosshair", "Slot)", "((PANEL", "self)(\.", ":))(ActionSignal", "ApplySchemeSettings", "DoClick", "Init", "OnCursorEntered", "OnCursorExited", "OnCursorMoved", "OnKeyCodePressed", "OnKeyCodeReleased", "OnKeyCodeTyped", "OnMousePressed", "OnMouseReleased", "OnMouseWheeled", "Paint", "PaintOver", "PerformLayout", "Think)", "((EFFECT", "self)(\.", ":))(Init", "Think", "Render)", "((SWEP", "self)(\.", ":))(AutoSwitchFrom", "WorldModel", "SetNPCMinBurst", "CanPrimaryAttack", "Spawnable", "SetNPCMaxRest", "PrintName", "AutoSwitchTo", "Base", "Instructions", "GetNPCMaxBurst", "TakePrimaryAmmo", "AcceptInput", "Holster", "GetNPCMinBurst", "Folder", "TakeSecondaryAmmo", "CanSecondaryAttack", "OnDrop", "EquipAmmo", "SetNPCFireRate", "SetupWeaponHoldTypeForAI", "OwnerChanged", "PrimaryAttack", "ClassName", "Contact", "Purpose", "Think", "SecondaryAttack", "AdminOnly", "SetWeaponHoldType", "Deploy", "Reload", "Ammo2", "NPCShoot_Secondary", "Initialize", "OnRemove", "ShootBullet", "Ammo1", "KeyValue", "ViewModel", "Equip", "SetDeploySpeed", "Weight", "ShouldDropOnDie", "OnRestore", "GetNPCMaxRest", "SetNPCMinRest", "DoImpactEffect", "NPCShoot_Primary", "ViewModelFlip", "ShootEffects", "ViewModelFOV", "SetNPCMaxBurst", "TranslateActivity", "GetNPCFireRate", "GetCapabilities", "GetNPCMinRest", "Author", "AdjustMouseSensitivity", "WepSelectIcon", "SpeechBubbleLid", "CustomAmmoDisplay", "DrawAmmo", "DrawWeaponInfoBox", "BobScale", "GetViewModelPosition", "RenderGroup", "DrawWorldModelTranslucent", "DrawHUD", "DrawWorldModel", "TranslateFOV", "SlotPos", "ViewModelDrawn", "DrawCrosshair", "FireAnimationEvent", "FreezeMovement", "SwayScale", "BounceWeaponIcon", "PrintWeaponInfo", "DrawWeaponSelection", "GetTracerOrigin", "Slot)", "((ENT", "self)(\.", ":))(AutomaticFrameAdvance", "Initialize", "PhysicsUpdate", "Purpose", "Spawnable", "SpawnFunction", "PrintName", "AdminOnly", "KeyValue", "Instructions", "StartTouch", "OnRestore", "AcceptInput", "OnTakeDamage", "Author", "Folder", "Use", "Think", "PhysicsCollide", "Type", "SetAutomaticFrameAdvance", "TriggerOutput", "StoreOutput", "Touch", "ClassName", "Contact", "Base", "OnRemove", "PhysicsSimulate", "EndTouch", "UpdateTransmitState", "RunAI", "OnActiveWeaponChanged", "m_iClass", "OnCondition", "StartTask", "RunTask", "OnMovementComplete", "SetMaxYawSpeed", "GetNPCClass", "StartEngineSchedule", "m_fMaxYawSpeed", "NextTask", "Task_PlaySequence", "TaskTime", "GetMaxYawSpeed", "EngineScheduleFinish", "SetNPCClass", "SelectSchedule", "GetAttackSpread", "TaskStart_PlaySequence", "OnTaskComplete", "OnChangeActivity", "StartEngineTask", "OnMovementFailed", "ExpressionFinished", "GetRelationship", "TaskFinished", "Task_FindEnemy", "TaskStart_FindEnemy", "StartSchedule", "DoingEngineSchedule", "ScheduleFinished", "DoSchedule", "SetTask", "RunEngineTask", "Draw", "GetGrabPos", "SetSize", "RenderGroup", "OnClick", "PressedThink", "TestCollision", "PressedThinkInternal", "Widget", "PressedShouldDraw", "PressEnd", "IsHovered", "GetSize", "OnRelease", "SetupDataTables", "IsPressed", "SomethingHovered", "OverlayRender", "OnRightClick", "PressStart", "OnPress", "DragThink", "Category", "PassesTriggerFilters", "DrawTranslucent", "MakeLight", "TimedCos", "istable", "SetGlobal2Float", "SetGlobalVector", "setfenv", "MakeEmitter", "GMODSpawnRagdoll", "DeriveGamemode", "ColorToHSV", "GetConVarNumber", "LocalToWorld", "MsgAll", "ismatrix", "STNDRD", "GetGlobalFloat", "IncludeCS", "RunConsoleCommand", "LerpVector", "RealTime", "CreateConVar", "MakeWheel", "GetGlobalEntity", "GetGlobalBool", "newproxy", "GetGlobal2String", "WorldToLocal", "MakeEffect", "gcinfo", "GetConVar", "AddOriginToPVS", "GetGlobal2Int", "Vector", "MakeDynamite", "GetGlobalAngle", "Path", "CreateClientConVar", "AccessorFunc", "tobool", "OrderVectors", "Model", "SoundDuration", "GetGlobal2Vector", "FindMetaTable", "Format", "isangle", "MakeLamp", "Lerp", "Spawn_NPC", "RandomPairs", "CreateSound", "CC_GMOD_Tool", "MakeThruster", "MakeHoverBall", "CC_Face_Randomize", "MakeButton", "Spawn_Vehicle", "Spawn_Weapon", "AddConsoleCommand", "TypeID", "include", "Spawn_SENT", "DoPlayerEntitySpawn", "GMODSpawnProp", "isnumber", "MakeProp", "DoPropSpawnedEffect", "Angle", "CheckPropSolid", "ServerLog", "getEnumerations", "TauntCamera", "SortedPairsByMemberValue", "SortedPairsByValue", "SortedPairs", "GMODSpawnEffect", "SQLStr", "GetConVarString", "Add_NPC_Class", "Either", "IsMounted", "IsFriendEntityName", "IsEnemyEntityName", "TimedSin", "UTIL_IsUselessModel", "IsUselessModel", "GetHostName", "SafeRemoveEntityDelayed", "SafeRemoveEntity", "IsValid", "Particle", "Sound", "ColorRand", "AngleRand", "VectorRand", "IsColor", "RecipientFilter", "EmitSound", "EmitSentence", "SentenceDuration", "CreatePhysCollidesFromModel", "CreatePhysCollideBox", "SetGlobal2Var", "GetGlobal2Entity", "ProtectedCall", "GetGlobal2Angle", "SetGlobal2Angle", "SetGlobal2Vector", "SetGlobal2String", "GetGlobal2Bool", "SetGlobal2Bool", "GetGlobal2Float", "SetGlobal2Int", "BuildNetworkedVarsTable", "GetGlobalString", "GetGlobalVector", "GetGlobalVar", "SetGlobalBool", "SetGlobalEntity", "SetGlobalAngle", "SetGlobalFloat", "SetGlobalInt", "SetGlobalString", "SetGlobalVar", "GetGlobalInt", "PrintMessage", "SetPhysConstraintSystem", "IsEntity", "AddCSLuaFile", "HTTP", "PrecacheSentenceGroup", "PrecacheSentenceFile", "ParticleEffectAttach", "ParticleEffect", "CompileFile", "GetPredictionPlayer", "FrameTime", "CurTime", "UnPredictedCurTime", "SuppressHostEvents", "PrecacheScene", "DropEntityIfHeld", "ispanel", "DTVar_ReceiveProxyGL", "isfunction", "isbool", "isvector", "FixInvalidPhysicsObject", "CompileString", "RunStringEx", "ColorToHSL", "HSLToColor", "GetConVar_Internal", "ConVarExists", "CCGiveSWEP", "LerpAngle", "ErrorNoHalt", "MsgN", "DebugInfo", "SysTime", "BroadcastLua", "VGUIFrameTime", "RunString", "SendUserMessage", "PrintTable", "SetGlobal2Entity", "MakeBalloon", "Player", "IsFirstTimePredicted", "Msg", "GetGlobal2Var", "getfenv", "DamageInfo", "Material", "Entity", "CCSpawn", "ColorAlpha", "Matrix", "HSVToColor", "isentity", "ErrorNoHaltWithStack", "Error", "PrecacheParticleSystem", "Color", "EffectData", "isstring", "IsTableOfEntitiesValid", "MsgC", "RestoreCursorPosition", "DrawMotionBlur", "DermaMenu", "AddonMaterial", "JS_Language", "Derma_DrawBackgroundBlur", "EyeVector", "ChangeTooltip", "DrawMaterialOverlay", "DynamicLight", "RenderSuperDoF", "DOF_Start", "ScrH", "DrawBloom", "TextEntryLoseFocus", "DrawSharpen", "ScrW", "GetRenderTarget", "ProjectedTexture", "EyePos", "NumModelSkins", "ClientsideRagdoll", "WorkshopFileBase", "AddWorldTip", "GenerateSpawnlistFromPath", "CreateContextMenu", "JS_Workshop", "CreateSprite", "VGUIRect", "ScreenScale", "Label", "DrawTexturize", "RenderDoF", "RenderStereoscopy", "DrawSobel", "DOF_Kill", "DrawColorModify", "Derma_Hook", "EndTooltip", "Derma_StringRequest", "Derma_Query", "Derma_Anim", "RegisterDermaMenuForClose", "ValidPanel", "SScale", "PositionSpawnIcon", "RealFrameTime", "FindTooltip", "RemoveTooltip", "VisualizeLayout", "JS_Utility", "RememberCursorPosition", "SetClipboardText", "NamedColor", "ParticleEmitter", "Mesh", "GetRenderTargetEx", "GetTimeoutInfo", "RenderAngles", "EyeAngles", "SavePresets", "LoadPresets", "ClientsideModel", "CreateParticleSystem", "CreateMaterial", "GetViewEntity", "Localize", "DOFModeHack", "LocalPlayer", "Derma_Install_Convar_Functions", "DrawToyTown", "AddPropsOfParent", "DisableClipping", "OnModelLoaded", "FrameNumber", "Derma_Message", "GetHUDPanel", "ClientsideScene", "DrawSunbeams", "CloseDermaMenus"
		};
		for (auto& k : gLua)
		{
			Identifier id;
			id.mDeclaration = "gLua Function";
			langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
		}

		langDef.mTokenize = [](const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end, PaletteIndex& paletteIndex) -> bool
		{
			paletteIndex = PaletteIndex::Max;

			while (in_begin < in_end && isascii(*in_begin) && isblank(*in_begin))
				in_begin++;

			if (in_begin == in_end)
			{
				out_begin = in_end;
				out_end = in_end;
				paletteIndex = PaletteIndex::Default;
			}
			else if (TokenizeCStyleString(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::String;
			else if (TokenizeCStyleCharacterLiteral(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::String;
			else if (TokenizeCStyleIdentifier(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Identifier;
			else if (TokenizeCStyleNumber(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Number;
			else if (TokenizeCStylePunctuation(in_begin, in_end, out_begin, out_end))
				paletteIndex = PaletteIndex::Punctuation;

			return paletteIndex != PaletteIndex::Max;
		};

		langDef.mCommentStart = "--[[";
		langDef.mCommentEnd = "]]";
		langDef.mCommentStart2 = "/*";
		langDef.mCommentEnd2 = "*/";
		langDef.mSingleLineComment = "--";
		langDef.mSingleLineComment2 = "//";

		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = true;

		langDef.mName = "C++";

		inited = true;
	}
	return langDef;
}