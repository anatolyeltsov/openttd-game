/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file picker_gui.cpp %File for dealing with picker windows */

#include "stdafx.h"
#include "core/backup_type.hpp"
#include "company_func.h"
#include "gui.h"
#include "hotkeys.h"
#include "ini_type.h"
#include "newgrf_badge.h"
#include "newgrf_badge_config.h"
#include "newgrf_badge_gui.h"
#include "picker_gui.h"
#include "querystring_gui.h"
#include "settings_type.h"
#include "sortlist_type.h"
#include "sound_func.h"
#include "sound_type.h"
#include "string_func.h"
#include "stringfilter_type.h"
#include "strings_func.h"
#include "widget_type.h"
#include "window_func.h"
#include "window_gui.h"
#include "window_type.h"
#include "zoom_func.h"

#include "widgets/picker_widget.h"

#include "table/sprites.h"
#include "table/strings.h"

#include <charconv>

#include "safeguards.h"

static std::vector<PickerCallbacks *> &GetPickerCallbacks()
{
	static std::vector<PickerCallbacks *> callbacks;
	return callbacks;
}

PickerCallbacks::PickerCallbacks(const std::string &ini_group) : ini_group(ini_group)
{
	GetPickerCallbacks().push_back(this);
}

PickerCallbacks::~PickerCallbacks()
{
	auto &callbacks = GetPickerCallbacks();
	callbacks.erase(std::ranges::find(callbacks, this));
}

/**
 * Load favourites of a picker from config.
 * @param ini IniFile to load to.
 * @param callbacks Picker to load.
 */
static void PickerLoadConfig(const IniFile &ini, PickerCallbacks &callbacks)
{
	const IniGroup *group = ini.GetGroup(callbacks.ini_group);
	if (group == nullptr) return;

	callbacks.saved.clear();
	for (const IniItem &item : group->items) {
		std::array<uint8_t, 4> grfid_buf;

		std::string_view str = item.name;

		/* Try reading "<grfid>|<localid>" */
		auto grfid_pos = str.find('|');
		if (grfid_pos == std::string_view::npos) continue;

		std::string_view grfid_str = str.substr(0, grfid_pos);
		if (!ConvertHexToBytes(grfid_str, grfid_buf)) continue;

		str = str.substr(grfid_pos + 1);
		uint32_t grfid = grfid_buf[0] | (grfid_buf[1] << 8) | (grfid_buf[2] << 16) | (grfid_buf[3] << 24);
		uint16_t localid;
		auto [ptr, err] = std::from_chars(str.data(), str.data() + str.size(), localid);

		if (err == std::errc{} && ptr == str.data() + str.size()) {
			callbacks.saved.insert({grfid, localid, 0, 0});
		}
	}
}

/**
 * Save favourites of a picker to config.
 * @param ini IniFile to save to.
 * @param callbacks Picker to save.
 */
static void PickerSaveConfig(IniFile &ini, const PickerCallbacks &callbacks)
{
	IniGroup &group = ini.GetOrCreateGroup(callbacks.ini_group);
	group.Clear();

	for (const PickerItem &item : callbacks.saved) {
		std::string key = fmt::format("{:08X}|{}", std::byteswap(item.grfid), item.local_id);
		group.CreateItem(key);
	}
}

/**
 * Load favourites of all registered Pickers from config.
 * @param ini IniFile to load to.
 */
void PickerLoadConfig(const IniFile &ini)
{
	for (auto *cb : GetPickerCallbacks()) PickerLoadConfig(ini, *cb);
}

/**
 * Save favourites of all registered Pickers to config.
 * @param ini IniFile to save to.
 */
void PickerSaveConfig(IniFile &ini)
{
	for (const auto *cb : GetPickerCallbacks()) PickerSaveConfig(ini, *cb);
}

/** Sort classes by id. */
static bool ClassIDSorter(int const &a, int const &b)
{
	return a < b;
}

/** Filter classes by class name. */
static bool ClassTagNameFilter(int const *item, PickerFilterData &filter)
{
	filter.ResetState();
	filter.AddLine(GetString(filter.callbacks->GetClassName(*item)));
	return filter.GetState();
}

/** Sort types by id. */
static bool TypeIDSorter(PickerItem const &a, PickerItem const &b)
{
	int r = a.class_index - b.class_index;
	if (r == 0) r = a.index - b.index;
	return r < 0;
}

/** Filter types by class name. */
static bool TypeTagNameFilter(PickerItem const *item, PickerFilterData &filter)
{
	auto badges = filter.callbacks->GetTypeBadges(item->class_index, item->index);
	if (filter.bdf.has_value() && !filter.bdf->Filter(badges)) return false;
	if (filter.btf.has_value() && filter.btf->Filter(badges)) return true;

	filter.ResetState();
	filter.AddLine(GetString(filter.callbacks->GetTypeName(item->class_index, item->index)));
	return filter.GetState();
}

static const std::initializer_list<PickerClassList::SortFunction * const> _class_sorter_funcs = { &ClassIDSorter }; ///< Sort functions of the #PickerClassList
static const std::initializer_list<PickerClassList::FilterFunction * const> _class_filter_funcs = { &ClassTagNameFilter }; ///< Filter functions of the #PickerClassList.
static const std::initializer_list<PickerTypeList::SortFunction * const> _type_sorter_funcs = { TypeIDSorter }; ///< Sort functions of the #PickerTypeList.
static const std::initializer_list<PickerTypeList::FilterFunction * const> _type_filter_funcs = { TypeTagNameFilter }; ///< Filter functions of the #PickerTypeList.

PickerWindow::PickerWindow(WindowDesc &desc, Window *parent, int window_number, PickerCallbacks &callbacks) : PickerWindowBase(desc, parent), callbacks(callbacks),
	class_editbox(EDITBOX_MAX_SIZE * MAX_CHAR_LENGTH, EDITBOX_MAX_SIZE),
	type_editbox(EDITBOX_MAX_SIZE * MAX_CHAR_LENGTH, EDITBOX_MAX_SIZE)
{
	this->window_number = window_number;

	/* Init of nested tree is deferred.
	 * PickerWindow::ConstructWindow must be called by the inheriting window. */
}

void PickerWindow::ConstructWindow()
{
	this->CreateNestedTree();

	/* Test if pickers should be active.*/
	bool is_active = this->callbacks.IsActive();

	this->preview_height = std::max(this->callbacks.preview_height, PREVIEW_HEIGHT);

	/* Functionality depends on widgets being present, not window class. */
	this->has_class_picker = is_active && this->GetWidget<NWidgetBase>(WID_PW_CLASS_LIST) != nullptr && this->callbacks.HasClassChoice();
	this->has_type_picker = is_active && this->GetWidget<NWidgetBase>(WID_PW_TYPE_MATRIX) != nullptr;

	if (this->has_class_picker) {
		this->GetWidget<NWidgetCore>(WID_PW_CLASS_LIST)->SetToolTip(this->callbacks.GetClassTooltip());

		this->querystrings[WID_PW_CLASS_FILTER] = &this->class_editbox;
	} else {
		if (auto *nwid = this->GetWidget<NWidgetStacked>(WID_PW_CLASS_SEL); nwid != nullptr) {
			/* Check the container orientation. MakeNWidgets adds an additional NWID_VERTICAL container so we check the grand-parent. */
			bool is_vertical = (nwid->parent->parent->type == NWID_VERTICAL);
			nwid->SetDisplayedPlane(is_vertical ? SZSP_HORIZONTAL : SZSP_VERTICAL);
		}
	}

	this->class_editbox.cancel_button = QueryString::ACTION_CLEAR;
	this->class_string_filter.SetFilterTerm(this->class_editbox.text.GetText());
	this->class_string_filter.callbacks = &this->callbacks;

	this->classes.SetListing(this->callbacks.class_last_sorting);
	this->classes.SetFiltering(this->callbacks.class_last_filtering);
	this->classes.SetSortFuncs(_class_sorter_funcs);
	this->classes.SetFilterFuncs(_class_filter_funcs);

	/* Update saved type information. */
	this->callbacks.saved = this->callbacks.UpdateSavedItems(this->callbacks.saved);

	/* Clear used type information. */
	this->callbacks.used.clear();

	if (this->has_type_picker) {
		/* Populate used type information. */
		this->callbacks.FillUsedItems(this->callbacks.used);

		SetWidgetDisabledState(WID_PW_MODE_ALL, !this->callbacks.HasClassChoice());

		this->GetWidget<NWidgetCore>(WID_PW_TYPE_ITEM)->SetToolTip(this->callbacks.GetTypeTooltip());

		auto *matrix = this->GetWidget<NWidgetMatrix>(WID_PW_TYPE_MATRIX);
		matrix->SetScrollbar(this->GetScrollbar(WID_PW_TYPE_SCROLL));

		this->querystrings[WID_PW_TYPE_FILTER] = &this->type_editbox;
	} else {
		if (auto *nwid = this->GetWidget<NWidgetStacked>(WID_PW_TYPE_SEL); nwid != nullptr) {
			/* Check the container orientation. MakeNWidgets adds an additional NWID_VERTICAL container so we check the grand-parent. */
			bool is_vertical = (nwid->parent->parent->type == NWID_VERTICAL);
			nwid->SetDisplayedPlane(is_vertical ? SZSP_HORIZONTAL : SZSP_VERTICAL);
		}
	}

	this->type_editbox.cancel_button = QueryString::ACTION_CLEAR;
	this->type_string_filter.SetFilterTerm(this->type_editbox.text.GetText());
	this->type_string_filter.callbacks = &this->callbacks;

	this->types.SetListing(this->callbacks.type_last_sorting);
	this->types.SetFiltering(this->callbacks.type_last_filtering);
	this->types.SetSortFuncs(_type_sorter_funcs);
	this->types.SetFilterFuncs(_type_filter_funcs);

	this->FinishInitNested(this->window_number);

	this->InvalidateData(PICKER_INVALIDATION_ALL);
}

void PickerWindow::OnInit()
{
	this->badge_classes = GUIBadgeClasses(this->callbacks.GetFeature());

	auto container = this->GetWidget<NWidgetContainer>(WID_PW_BADGE_FILTER);
	this->badge_filters = AddBadgeDropdownFilters(*container, WID_PW_BADGE_FILTER, COLOUR_DARK_GREEN, this->callbacks.GetFeature());

	this->widget_lookup.clear();
	this->nested_root->FillWidgetLookup(this->widget_lookup);
}

void PickerWindow::Close(int data)
{
	this->callbacks.Close(data);
	this->PickerWindowBase::Close(data);
}

void PickerWindow::UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize)
{
	switch (widget) {
		/* Class picker */
		case WID_PW_CLASS_LIST:
			fill.height = resize.height = GetCharacterHeight(FS_NORMAL) + padding.height;
			size.height = 5 * resize.height;
			break;

		/* Type picker */
		case WID_PW_TYPE_MATRIX:
			/* At least two items wide. */
			size.width += resize.width;
			fill.width = resize.width;
			fill.height = 1;

			/* Resizing in X direction only at blob size, but at pixel level in Y. */
			resize.height = 1;
			break;

		/* Type picker */
		case WID_PW_TYPE_ITEM:
			size.width  = ScaleGUITrad(PREVIEW_WIDTH) + WidgetDimensions::scaled.fullbevel.Horizontal();
			size.height = ScaleGUITrad(this->preview_height) + WidgetDimensions::scaled.fullbevel.Vertical();
			break;

		case WID_PW_CONFIGURE_BADGES:
			/* Hide the configuration button if no configurable badges are present. */
			if (this->badge_classes.GetClasses().empty()) size = {0, 0};
			break;
	}
}

std::string PickerWindow::GetWidgetString(WidgetID widget, StringID stringid) const
{
	if (IsInsideMM(widget, this->badge_filters.first, this->badge_filters.second)) {
		return this->GetWidget<NWidgetBadgeFilter>(widget)->GetStringParameter(this->badge_filter_choices);
	}

	return this->Window::GetWidgetString(widget, stringid);
}

void PickerWindow::DrawWidget(const Rect &r, WidgetID widget) const
{
	switch (widget) {
		/* Class picker */
		case WID_PW_CLASS_LIST: {
			Rect ir = r.Shrink(WidgetDimensions::scaled.matrix);
			const int selected = this->callbacks.GetSelectedClass();
			const auto vscroll = this->GetScrollbar(WID_PW_CLASS_SCROLL);
			const int y_step = this->GetWidget<NWidgetResizeBase>(widget)->resize_y;
			auto [first, last] = vscroll->GetVisibleRangeIterators(this->classes);
			for (auto it = first; it != last; ++it) {
				DrawString(ir, this->callbacks.GetClassName(*it), *it == selected ? TC_WHITE : TC_BLACK);
				ir.top += y_step;
			}
			break;
		}

		/* Type picker */
		case WID_PW_TYPE_ITEM: {
			assert(this->GetWidget<NWidgetBase>(widget)->GetParentWidget<NWidgetMatrix>()->GetCurrentElement() < static_cast<int>(this->types.size()));
			const auto &item = this->types[this->GetWidget<NWidgetBase>(widget)->GetParentWidget<NWidgetMatrix>()->GetCurrentElement()];

			DrawPixelInfo tmp_dpi;
			Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
			if (FillDrawPixelInfo(&tmp_dpi, ir)) {
				AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
				int x = (ir.Width()  - ScaleSpriteTrad(PREVIEW_WIDTH)) / 2 + ScaleSpriteTrad(PREVIEW_LEFT);
				int y = (ir.Height() + ScaleSpriteTrad(this->preview_height)) / 2 - ScaleSpriteTrad(PREVIEW_BOTTOM);

				this->callbacks.DrawType(x, y, item.class_index, item.index);

				int by = ir.Height() - ScaleGUITrad(12);

				GrfSpecFeature feature = this->callbacks.GetFeature();
				DrawBadgeColumn({0, by, ir.Width() - 1, ir.Height() - 1}, 0, this->badge_classes, this->callbacks.GetTypeBadges(item.class_index, item.index), feature, std::nullopt, PAL_NONE);

				if (this->callbacks.saved.contains(item)) {
					DrawSprite(SPR_BLOT, PALETTE_TO_YELLOW, 0, 0);
				}
				if (this->callbacks.used.contains(item)) {
					DrawSprite(SPR_BLOT, PALETTE_TO_GREEN, ir.Width() - GetSpriteSize(SPR_BLOT).width, 0);
				}
			}

			if (!this->callbacks.IsTypeAvailable(item.class_index, item.index)) {
				GfxFillRect(ir, GetColourGradient(COLOUR_GREY, SHADE_DARKER), FILLRECT_CHECKER);
			}
			break;
		}

		case WID_PW_TYPE_NAME:
			DrawString(r, this->callbacks.GetTypeName(this->callbacks.GetSelectedClass(), this->callbacks.GetSelectedType()), TC_ORANGE, SA_CENTER);
			break;
	}
}

void PickerWindow::OnResize()
{
	if (this->has_class_picker) {
		this->GetScrollbar(WID_PW_CLASS_SCROLL)->SetCapacityFromWidget(this, WID_PW_CLASS_LIST);
	}
}

void PickerWindow::OnClick(Point pt, WidgetID widget, int)
{
	switch (widget) {
		/* Class Picker */
		case WID_PW_CLASS_LIST: {
			const auto vscroll = this->GetWidget<NWidgetScrollbar>(WID_PW_CLASS_SCROLL);
			auto it = vscroll->GetScrolledItemFromWidget(this->classes, pt.y, this, WID_PW_CLASS_LIST);
			if (it == this->classes.end()) return;

			if (this->callbacks.GetSelectedClass() != *it || HasBit(this->callbacks.mode, PFM_ALL)) {
				ClrBit(this->callbacks.mode, PFM_ALL); // Disable showing all.
				this->callbacks.SetSelectedClass(*it);
				this->InvalidateData({PickerInvalidation::Type, PickerInvalidation::Position, PickerInvalidation::Validate});
			}
			SndClickBeep();
			CloseWindowById(WC_SELECT_STATION, 0);
			break;
		}

		case WID_PW_MODE_ALL:
		case WID_PW_MODE_USED:
		case WID_PW_MODE_SAVED:
			ToggleBit(this->callbacks.mode, widget - WID_PW_MODE_ALL);
			if (!this->IsWidgetDisabled(WID_PW_MODE_ALL) && HasBit(this->callbacks.mode, widget - WID_PW_MODE_ALL)) {
				/* Enabling used or saved filters automatically enables all. */
				SetBit(this->callbacks.mode, PFM_ALL);
			}
			this->InvalidateData({PickerInvalidation::Class, PickerInvalidation::Type, PickerInvalidation::Position});
			break;

		case WID_PW_SHRINK:
			this->callbacks.preview_height = this->preview_height = _ctrl_pressed ? PREVIEW_HEIGHT : std::max(PREVIEW_HEIGHT, this->preview_height - STEP_PREVIEW_HEIGHT);
			this->InvalidateData({});
			this->ReInit();
			break;

		case WID_PW_EXPAND:
			this->callbacks.preview_height = this->preview_height = _ctrl_pressed ? MAX_PREVIEW_HEIGHT : std::min(MAX_PREVIEW_HEIGHT, this->preview_height + STEP_PREVIEW_HEIGHT);
			this->InvalidateData({});
			this->ReInit();
			break;

		/* Type Picker */
		case WID_PW_TYPE_ITEM: {
			int sel = this->GetWidget<NWidgetBase>(widget)->GetParentWidget<NWidgetMatrix>()->GetCurrentElement();
			assert(sel < (int)this->types.size());
			const auto &item = this->types[sel];

			if (_ctrl_pressed) {
				auto it = this->callbacks.saved.find(item);
				if (it == std::end(this->callbacks.saved)) {
					this->callbacks.saved.insert(item);
				} else {
					this->callbacks.saved.erase(it);
				}
				this->InvalidateData(PickerInvalidation::Type);
				break;
			}

			if (this->callbacks.IsTypeAvailable(item.class_index, item.index)) {
				this->callbacks.SetSelectedClass(item.class_index);
				this->callbacks.SetSelectedType(item.index);
				this->InvalidateData(PickerInvalidation::Position);
			}
			SndClickBeep();
			CloseWindowById(WC_SELECT_STATION, 0);
			break;
		}

		case WID_PW_CONFIGURE_BADGES:
			if (this->badge_classes.GetClasses().empty()) break;
			ShowDropDownList(this, BuildBadgeClassConfigurationList(this->badge_classes, 1, {}), -1, widget, 0, false, true);
			break;

		default:
			if (IsInsideMM(widget, this->badge_filters.first, this->badge_filters.second)) {
				ShowDropDownList(this, this->GetWidget<NWidgetBadgeFilter>(widget)->GetDropDownList(), -1, widget, 0, false);
			}
			break;
	}
}

void PickerWindow::OnDropdownSelect(WidgetID widget, int index, int click_result)
{
	switch (widget) {
		case WID_PW_CONFIGURE_BADGES: {
			bool reopen = HandleBadgeConfigurationDropDownClick(this->callbacks.GetFeature(), 1, index, click_result, this->badge_filter_choices);

			this->ReInit();

			if (reopen) {
				ReplaceDropDownList(this, BuildBadgeClassConfigurationList(this->badge_classes, 1, {}), -1);
			} else {
				this->CloseChildWindows(WC_DROPDOWN_MENU);
			}

			/* We need to refresh if a filter is removed. */
			this->InvalidateData({PickerInvalidation::Type, PickerInvalidation::Filter});
			break;
		}

		default:
			if (IsInsideMM(widget, this->badge_filters.first, this->badge_filters.second)) {
				if (index < 0) {
					ResetBadgeFilter(this->badge_filter_choices, this->GetWidget<NWidgetBadgeFilter>(widget)->GetBadgeClassID());
				} else {
					SetBadgeFilter(this->badge_filter_choices, BadgeID(index));
				}
				this->InvalidateData({PickerInvalidation::Type, PickerInvalidation::Filter});
			}
			break;
	}
}

void PickerWindow::OnInvalidateData(int data, bool gui_scope)
{
	if (!gui_scope) return;

	PickerInvalidations pi(data);

	if (pi.Test(PickerInvalidation::Filter)) {
		if (this->badge_filter_choices.empty()) {
			this->type_string_filter.bdf.reset();
		} else {
			this->type_string_filter.bdf.emplace(this->badge_filter_choices);
		}
		this->types.SetFilterState(!type_string_filter.IsEmpty() || type_string_filter.bdf.has_value());
	}

	if (pi.Test(PickerInvalidation::Class)) this->classes.ForceRebuild();
	if (pi.Test(PickerInvalidation::Type)) this->types.ForceRebuild();

	this->BuildPickerClassList();
	if (pi.Test(PickerInvalidation::Validate)) this->EnsureSelectedClassIsValid();
	if (pi.Test(PickerInvalidation::Position)) this->EnsureSelectedClassIsVisible();

	this->BuildPickerTypeList();
	if (pi.Test(PickerInvalidation::Validate)) this->EnsureSelectedTypeIsValid();
	if (pi.Test(PickerInvalidation::Position)) this->EnsureSelectedTypeIsVisible();

	if (this->has_type_picker) {
		SetWidgetLoweredState(WID_PW_MODE_ALL, HasBit(this->callbacks.mode, PFM_ALL));
		SetWidgetLoweredState(WID_PW_MODE_USED, HasBit(this->callbacks.mode, PFM_USED));
		SetWidgetLoweredState(WID_PW_MODE_SAVED, HasBit(this->callbacks.mode, PFM_SAVED));
	}

	SetWidgetDisabledState(WID_PW_SHRINK, this->preview_height == PREVIEW_HEIGHT);
	SetWidgetDisabledState(WID_PW_EXPAND, this->preview_height == MAX_PREVIEW_HEIGHT);
}

EventState PickerWindow::OnHotkey(int hotkey)
{
	switch (hotkey) {
		case PCWHK_FOCUS_FILTER_BOX:
			/* Cycle between the two edit boxes. */
			if (this->has_type_picker && (this->nested_focus == nullptr || this->nested_focus->GetIndex() != WID_PW_TYPE_FILTER)) {
				this->SetFocusedWidget(WID_PW_TYPE_FILTER);
			} else if (this->has_class_picker && (this->nested_focus == nullptr || this->nested_focus->GetIndex() != WID_PW_CLASS_FILTER)) {
				this->SetFocusedWidget(WID_PW_CLASS_FILTER);
			}
			SetFocusedWindow(this);
			return ES_HANDLED;

		default:
			return ES_NOT_HANDLED;
	}
}

void PickerWindow::OnEditboxChanged(WidgetID wid)
{
	switch (wid) {
		case WID_PW_CLASS_FILTER:
			this->class_string_filter.SetFilterTerm(this->class_editbox.text.GetText());
			this->classes.SetFilterState(!class_string_filter.IsEmpty());
			this->InvalidateData(PickerInvalidation::Class);
			break;

		case WID_PW_TYPE_FILTER:
			this->type_string_filter.SetFilterTerm(this->type_editbox.text.GetText());
			if (!type_string_filter.IsEmpty()) {
				this->type_string_filter.btf.emplace(this->type_string_filter, this->callbacks.GetFeature());
			} else {
				this->type_string_filter.btf.reset();
			}
			this->InvalidateData({PickerInvalidation::Type, PickerInvalidation::Filter});
			break;

		default:
			break;
	}
}

/** Builds the filter list of classes. */
void PickerWindow::BuildPickerClassList()
{
	if (!this->classes.NeedRebuild()) return;

	int count = this->callbacks.GetClassCount();

	this->classes.clear();
	this->classes.reserve(count);

	bool filter_used = HasBit(this->callbacks.mode, PFM_USED);
	bool filter_saved = HasBit(this->callbacks.mode, PFM_SAVED);
	for (int i = 0; i < count; i++) {
		if (this->callbacks.GetClassName(i) == INVALID_STRING_ID) continue;
		if (filter_used && std::none_of(std::begin(this->callbacks.used), std::end(this->callbacks.used), [i](const PickerItem &item) { return item.class_index == i; })) continue;
		if (filter_saved && std::none_of(std::begin(this->callbacks.saved), std::end(this->callbacks.saved), [i](const PickerItem &item) { return item.class_index == i; })) continue;
		this->classes.emplace_back(i);
	}

	this->classes.Filter(this->class_string_filter);
	this->classes.RebuildDone();
	this->classes.Sort();

	if (!this->has_class_picker) return;
	this->GetScrollbar(WID_PW_CLASS_SCROLL)->SetCount(this->classes.size());
}

void PickerWindow::EnsureSelectedClassIsValid()
{
	int class_index = this->callbacks.GetSelectedClass();
	if (std::binary_search(std::begin(this->classes), std::end(this->classes), class_index)) return;

	if (!this->classes.empty()) {
		class_index = this->classes.front();
	} else {
		/* Classes can be empty if filters are enabled, find the first usable class. */
		int count = this->callbacks.GetClassCount();
		for (int i = 0; i < count; i++) {
			if (this->callbacks.GetClassName(i) == INVALID_STRING_ID) continue;
			class_index = i;
			break;
		}
	}

	this->callbacks.SetSelectedClass(class_index);
	this->types.ForceRebuild();
}

void PickerWindow::EnsureSelectedClassIsVisible()
{
	if (!this->has_class_picker) return;
	if (this->classes.empty()) return;

	auto it = std::ranges::find(this->classes, this->callbacks.GetSelectedClass());
	if (it == std::end(this->classes)) return;

	int pos = static_cast<int>(std::distance(std::begin(this->classes), it));
	this->GetScrollbar(WID_PW_CLASS_SCROLL)->ScrollTowards(pos);
}

void PickerWindow::RefreshUsedTypeList()
{
	if (!this->has_type_picker) return;

	this->callbacks.used.clear();
	this->callbacks.FillUsedItems(this->callbacks.used);
	this->InvalidateData(PickerInvalidation::Type);
}

/** Builds the filter list of types. */
void PickerWindow::BuildPickerTypeList()
{
	if (!this->types.NeedRebuild()) return;

	this->types.clear();

	bool show_all = HasBit(this->callbacks.mode, PFM_ALL);
	bool filter_used = HasBit(this->callbacks.mode, PFM_USED);
	bool filter_saved = HasBit(this->callbacks.mode, PFM_SAVED);
	int cls_id = this->callbacks.GetSelectedClass();

	if (filter_used) {
		/* Showing used items. May also be filtered by saved items. */
		this->types.reserve(this->callbacks.used.size());
		for (const PickerItem &item : this->callbacks.used) {
			if (!show_all && item.class_index != cls_id) continue;
			if (this->callbacks.GetTypeName(item.class_index, item.index) == INVALID_STRING_ID) continue;
			this->types.emplace_back(item);
		}
	} else if (filter_saved) {
		/* Showing only saved items. */
		this->types.reserve(this->callbacks.saved.size());
		for (const PickerItem &item : this->callbacks.saved) {
			/* The used list may contain items that aren't currently loaded, skip these. */
			if (item.class_index == -1) continue;
			if (!show_all && item.class_index != cls_id) continue;
			if (this->callbacks.GetTypeName(item.class_index, item.index) == INVALID_STRING_ID) continue;
			this->types.emplace_back(item);
		}
	} else if (show_all) {
		/* Reserve enough space for everything. */
		int total = 0;
		for (int class_index : this->classes) total += this->callbacks.GetTypeCount(class_index);
		this->types.reserve(total);
		/* Add types in all classes. */
		for (int class_index : this->classes) {
			int count = this->callbacks.GetTypeCount(class_index);
			for (int i = 0; i < count; i++) {
				if (this->callbacks.GetTypeName(class_index, i) == INVALID_STRING_ID) continue;
				this->types.emplace_back(this->callbacks.GetPickerItem(class_index, i));
			}
		}
	} else {
		/* Add types in only the selected class. */
		if (cls_id >= 0 && cls_id < this->callbacks.GetClassCount()) {
			int count = this->callbacks.GetTypeCount(cls_id);
			this->types.reserve(count);
			for (int i = 0; i < count; i++) {
				if (this->callbacks.GetTypeName(cls_id, i) == INVALID_STRING_ID) continue;
				this->types.emplace_back(this->callbacks.GetPickerItem(cls_id, i));
			}
		}
	}

	this->types.Filter(this->type_string_filter);
	this->types.RebuildDone();
	this->types.Sort();

	if (!this->has_type_picker) return;
	this->GetWidget<NWidgetMatrix>(WID_PW_TYPE_MATRIX)->SetCount(static_cast<int>(this->types.size()));
}

void PickerWindow::EnsureSelectedTypeIsValid()
{
	int class_index = this->callbacks.GetSelectedClass();
	int index = this->callbacks.GetSelectedType();
	if (std::any_of(std::begin(this->types), std::end(this->types), [class_index, index](const auto &item) { return item.class_index == class_index && item.index == index; })) return;

	if (!this->types.empty()) {
		class_index = this->types.front().class_index;
		index = this->types.front().index;
	} else {
		/* Types can be empty if filters are enabled, find the first usable type. */
		int count = this->callbacks.GetTypeCount(class_index);
		for (int i = 0; i < count; i++) {
			if (this->callbacks.GetTypeName(class_index, i) == INVALID_STRING_ID) continue;
			index = i;
			break;
		}
	}
	this->callbacks.SetSelectedClass(class_index);
	this->callbacks.SetSelectedType(index);
}

void PickerWindow::EnsureSelectedTypeIsVisible()
{
	if (!this->has_type_picker) return;
	if (this->types.empty()) {
		this->GetWidget<NWidgetMatrix>(WID_PW_TYPE_MATRIX)->SetClicked(-1);
		return;
	}

	int class_index = this->callbacks.GetSelectedClass();
	int index = this->callbacks.GetSelectedType();

	auto it = std::ranges::find_if(this->types, [class_index, index](const auto &item) { return item.class_index == class_index && item.index == index; });
	if (it == std::end(this->types)) return;

	int pos = static_cast<int>(std::distance(std::begin(this->types), it));
	this->GetWidget<NWidgetMatrix>(WID_PW_TYPE_MATRIX)->SetClicked(pos);
}

/** Create nested widgets for the class picker widgets. */
std::unique_ptr<NWidgetBase> MakePickerClassWidgets()
{
	static constexpr NWidgetPart picker_class_widgets[] = {
		NWidget(NWID_SELECTION, INVALID_COLOUR, WID_PW_CLASS_SEL),
			NWidget(NWID_VERTICAL),
				NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
					NWidget(WWT_EDITBOX, COLOUR_DARK_GREEN, WID_PW_CLASS_FILTER), SetMinimalSize(144, 0), SetPadding(2), SetFill(1, 0), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
						NWidget(WWT_MATRIX, COLOUR_GREY, WID_PW_CLASS_LIST), SetFill(1, 1), SetResize(1, 1), SetPadding(WidgetDimensions::unscaled.picker),
								SetMatrixDataTip(1, 0), SetScrollbar(WID_PW_CLASS_SCROLL),
					EndContainer(),
					NWidget(NWID_VSCROLLBAR, COLOUR_DARK_GREEN, WID_PW_CLASS_SCROLL),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	};

	return MakeNWidgets(picker_class_widgets, nullptr);
}

/** Create nested widgets for the type picker widgets. */
std::unique_ptr<NWidgetBase> MakePickerTypeWidgets()
{
	static constexpr NWidgetPart picker_type_widgets[] = {
		NWidget(NWID_SELECTION, INVALID_COLOUR, WID_PW_TYPE_SEL),
			NWidget(NWID_VERTICAL),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
						NWidget(WWT_EDITBOX, COLOUR_DARK_GREEN, WID_PW_TYPE_FILTER), SetPadding(2), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP),
					EndContainer(),
					NWidget(WWT_IMGBTN, COLOUR_DARK_GREEN, WID_PW_CONFIGURE_BADGES), SetAspect(WidgetDimensions::ASPECT_UP_DOWN_BUTTON), SetResize(0, 0), SetFill(0, 1), SetSpriteTip(SPR_EXTRA_MENU, STR_BADGE_CONFIG_MENU_TOOLTIP),
				EndContainer(),
				NWidget(NWID_VERTICAL, NWidContainerFlag{}, WID_PW_BADGE_FILTER),
			EndContainer(),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_PW_MODE_ALL), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_PICKER_MODE_ALL, STR_PICKER_MODE_ALL_TOOLTIP),
					NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_PW_MODE_USED), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_PICKER_MODE_USED, STR_PICKER_MODE_USED_TOOLTIP),
					NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_PW_MODE_SAVED), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_PICKER_MODE_SAVED, STR_PICKER_MODE_SAVED_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_DARK_GREEN, WID_PW_SHRINK), SetAspect(WidgetDimensions::ASPECT_UP_DOWN_BUTTON), SetStringTip(STR_PICKER_PREVIEW_SHRINK, STR_PICKER_PREVIEW_SHRINK_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, COLOUR_DARK_GREEN, WID_PW_EXPAND), SetAspect(WidgetDimensions::ASPECT_UP_DOWN_BUTTON), SetStringTip(STR_PICKER_PREVIEW_EXPAND, STR_PICKER_PREVIEW_EXPAND_TOOLTIP),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PANEL, COLOUR_DARK_GREEN), SetScrollbar(WID_PW_TYPE_SCROLL),
						NWidget(NWID_MATRIX, COLOUR_DARK_GREEN, WID_PW_TYPE_MATRIX), SetPIP(0, 2, 0), SetPadding(WidgetDimensions::unscaled.picker),
							NWidget(WWT_PANEL, COLOUR_GREY, WID_PW_TYPE_ITEM), SetScrollbar(WID_PW_TYPE_SCROLL),
							EndContainer(),
						EndContainer(),
					EndContainer(),
					NWidget(NWID_VSCROLLBAR, COLOUR_DARK_GREEN, WID_PW_TYPE_SCROLL),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
						NWidget(WWT_EMPTY, INVALID_COLOUR, WID_PW_TYPE_NAME), SetPadding(WidgetDimensions::unscaled.framerect), SetResize(1, 0), SetFill(1, 0), SetMinimalTextLines(1, 0),
					EndContainer(),
					NWidget(WWT_RESIZEBOX, COLOUR_DARK_GREEN, WID_PW_TYPE_RESIZE),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	};

	return MakeNWidgets(picker_type_widgets, nullptr);
}

void InvalidateAllPickerWindows()
{
	InvalidateWindowClassesData(WC_BUS_STATION, PickerWindow::PICKER_INVALIDATION_ALL);
	InvalidateWindowClassesData(WC_TRUCK_STATION, PickerWindow::PICKER_INVALIDATION_ALL);
	InvalidateWindowClassesData(WC_SELECT_STATION, PickerWindow::PICKER_INVALIDATION_ALL);
	InvalidateWindowClassesData(WC_BUILD_WAYPOINT, PickerWindow::PICKER_INVALIDATION_ALL);
	InvalidateWindowClassesData(WC_BUILD_OBJECT, PickerWindow::PICKER_INVALIDATION_ALL);
	InvalidateWindowClassesData(WC_BUILD_HOUSE, PickerWindow::PICKER_INVALIDATION_ALL);
}
