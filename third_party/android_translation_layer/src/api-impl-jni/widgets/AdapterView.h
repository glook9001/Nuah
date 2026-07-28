#ifndef _ADAPTER_VIEW_H_
#define _ADAPTER_VIEW_H_

#include <gtk/gtk.h>
#include <jni.h>

/*
 * RangeListModel:
 * Implementation of GListModel for use in AdapterView subclasses.
 * Provides RangeListItems, which are created on demand.
 */
struct _RangeListModel {
	GObject parent_instance;
	GtkWidget *list_view;
	jobject jobject;
	jobject adapter;
	guint n_items;
};
G_DECLARE_FINAL_TYPE(RangeListModel, range_list_model, RANGE, LIST_MODEL, GObject)

/*
 * RangeListItem:
 * Dummy type to be returned by RangeListModel. Contains nothing, but a reference to the RangeListModel.
 */
struct _RangeListItem {
	GObject parent_instance;
	RangeListModel *model;
};
G_DECLARE_FINAL_TYPE(RangeListItem, range_list_item, RANGE, LIST_ITEM, GObject)

#endif // _ADAPTER_VIEW_H_
