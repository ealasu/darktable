/*
    This file is part of darktable,
    copyright (c) 2012 aldric renaudin.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "common/debug.h"
#include "common/mipmap_cache.h"

#include "develop/masks/circle.c"
#include "develop/masks/path.c"
#include "develop/masks/group.c"

static void _set_hinter_message(dt_masks_form_gui_t *gui)
{
  char msg[256] = "";
  if (gui->creation) strcat(msg,_("ctrl+click to add a sharp node"));
  else if (gui->point_selected >= 0) strcat(msg,_("ctrl+click to switch between smooth/sharp node"));
  else if (gui->feather_selected >= 0) strcat(msg,_("right-click to reset feather value"));
  else if (gui->seg_selected >= 0) strcat(msg,_("ctrl+click to add a node"));
  else if (gui->form_selected) strcat(msg,_("ctrl+scroll to set shape opacity"));

  dt_control_hinter_message(darktable.control,msg);
}

void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  if (g_list_length(gui->points) == index)
  {
    dt_masks_form_gui_points_t *gpt2 = (dt_masks_form_gui_points_t *) malloc(sizeof(dt_masks_form_gui_points_t));
    gui->points = g_list_append(gui->points,gpt2);
  }
  else if (g_list_length(gui->points) < index) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  gui->pipe_hash = gui->formid = gpt->points_count = gpt->border_count = gpt->source_count = 0;
  gpt->points = gpt->border = gpt->source = NULL;

  if (dt_masks_get_points_border(darktable.develop,form, &gpt->points, &gpt->points_count,&gpt->border, &gpt->border_count,0))
  {
    if (form->type & DT_MASKS_CLONE) dt_masks_get_points_border(darktable.develop,form, &gpt->source, &gpt->source_count,NULL,NULL,1);
    gui->pipe_hash = darktable.develop->preview_pipe->backbuf_hash;
    gui->formid = form->formid;
  }
}
void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  gui->pipe_hash = gui->formid = gpt->points_count = gpt->border_count = gpt->source_count = 0;
  free(gpt->points);
  gpt->points = NULL;
  free(gpt->border);
  gpt->border = NULL;
  free(gpt->source);
  gpt->source = NULL;
}

void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  //we test if the image has changed
  if (gui->pipe_hash > 0)
  {
    if (gui->pipe_hash != darktable.develop->preview_pipe->backbuf_hash)
    {
      gui->pipe_hash = gui->formid = 0;
      g_list_free(gui->points);
    }
  }

  //we create the spots if needed
  if (gui->pipe_hash == 0)
  {
    if (form->type & DT_MASKS_GROUP)
    {
      GList *fpts = g_list_first(form->points);
      int pos = 0;
      while(fpts)
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *) fpts->data;
        dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop,fpt->formid);
        dt_masks_gui_form_create(sel,gui,pos);
        fpts = g_list_next(fpts);
        pos++;
      }
    }
    else dt_masks_gui_form_create(form,gui,0);
  }
}

void _check_id(dt_masks_form_t *form)
{
  GList *forms = g_list_first(darktable.develop->forms);
  int nid = 100;
  while (forms)
  {
    dt_masks_form_t *ff = (dt_masks_form_t *)forms->data;
    if (ff->formid == form->formid)
    {
      form->formid = nid++;
      forms = g_list_first(darktable.develop->forms);
      continue;
    }
    forms = g_list_next(forms);
  }
}

void dt_masks_gui_form_save_creation(dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  //we check if the id is already registered
  _check_id(form);

  darktable.develop->forms = g_list_append(darktable.develop->forms,form);
  if (gui) gui->creation = FALSE;

  int nb = g_list_length(darktable.develop->forms);

  if (form->type & DT_MASKS_CIRCLE) snprintf(form->name,128,_("circle #%d"),nb);
  else if (form->type & DT_MASKS_PATH) snprintf(form->name,128,_("path #%d"),nb);

  dt_masks_write_form(form,darktable.develop);

  if (module)
  {
    //is there already a masks group for this module ?
    int grpid = module->blend_params->mask_id;
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,grpid);
    if (!grp)
    {
      //we create a new group
      if (form->type & DT_MASKS_CLONE) grp = dt_masks_create(DT_MASKS_GROUP | DT_MASKS_CLONE);
      else grp = dt_masks_create(DT_MASKS_GROUP);
      snprintf(grp->name,128,"grp %s %s",module->name(),module->multi_name);
      _check_id(grp);
      darktable.develop->forms = g_list_append(darktable.develop->forms,grp);
      module->blend_params->mask_id = grpid = grp->formid;
    }
    //we add the form in this group
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grpid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if (g_list_length(grp->points)>0) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = 1.0f;
    grp->points = g_list_append(grp->points,grpt);
    //we save the group
    dt_masks_write_form(grp,darktable.develop);
    //we update module gui
    if (gui) dt_masks_iop_update(module);
  }
  //show the form if needed
  if (gui) darktable.develop->form_gui->formid = form->formid;
  if (gui) dt_dev_masks_list_change(darktable.develop);
}

int dt_masks_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count, float **border, int *border_count, int source)
{
  if (form->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
    float x,y;
    if (source) x=form->source[0], y=form->source[1];
    else x=circle->center[0], y=circle->center[1];
    if (dt_circle_get_points(dev,x, y, circle->radius, points, points_count))
    {
      if (border) return dt_circle_get_points(dev,x,y, circle->radius + circle->border, border, border_count);
      else return 1;
    }
  }
  else if (form->type & DT_MASKS_PATH)
  {
    return dt_path_get_points_border(dev,form, points, points_count, border, border_count,source);
  }
  return 0;
}

int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if (form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_area(module,piece,form,width,height,posx,posy);
  }
  else if (form->type & DT_MASKS_PATH)
  {
    return dt_path_get_area(module,piece,form,width,height,posx,posy);
  }
  return 0;
}

int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if (form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_source_area(module,piece,form,width,height,posx,posy);
  }
  else if (form->type & DT_MASKS_PATH)
  {
    return dt_path_get_source_area(module,piece,form,width,height,posx,posy);
  }
  return 0;
}

int dt_masks_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, float **buffer, int *width, int *height, int *posx, int *posy)
{
  if (form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_mask(module,piece,form,buffer,width,height,posx,posy);
  }
  else if (form->type & DT_MASKS_PATH)
  {
    return dt_path_get_mask(module,piece,form,buffer,width,height,posx,posy);
  }
  else if (form->type & DT_MASKS_GROUP)
  {
    return dt_group_get_mask(module,piece,form,buffer,width,height,posx,posy);
  }
  return 0;
}

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *form = (dt_masks_form_t *)malloc(sizeof(dt_masks_form_t));
  form->type = type;
  form->version = 1;
  form->formid = time(NULL);

  form->points = NULL;

  return form;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id)
{
  GList *forms = g_list_first(dev->forms);
  while (forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *) forms->data;
    if (form->formid == id) return form;
    forms = g_list_next(forms);
  }
  return NULL;
}


void dt_masks_read_forms(dt_develop_t *dev)
{
  //first we have to remove all existant entries from the list
  if (dev->forms)
  {
    GList *forms = g_list_first(dev->forms);
    while (forms)
    {
      dt_masks_free_form((dt_masks_form_t *)forms->data);
      forms = g_list_next(forms);
    }
    g_list_free(dev->forms);
    dev->forms = NULL;
  }

  if(dev->image_storage.id <= 0) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select imgid, formid, form, name, version, points, points_count, source from mask where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-formid, 2-form_type, 3-name, 4-version, 5-points, 6-points_count, 7-source

    //we get the values
    dt_masks_form_t *form = (dt_masks_form_t *)malloc(sizeof(dt_masks_form_t));
    form->formid = sqlite3_column_int(stmt, 1);
    form->type = sqlite3_column_int(stmt, 2);
    const char *name = (const char *)sqlite3_column_text(stmt, 3);
    snprintf(form->name,128,"%s",name);
    form->version = sqlite3_column_int(stmt, 4);
    form->points = NULL;
    int nb_points = sqlite3_column_int(stmt, 6);
    memcpy(form->source, sqlite3_column_blob(stmt, 7), 2*sizeof(float));

    //and now we "read" the blob
    if (form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)malloc(sizeof(dt_masks_point_circle_t));
      memcpy(circle, sqlite3_column_blob(stmt, 5), sizeof(dt_masks_point_circle_t));
      form->points = g_list_append(form->points,circle);
    }
    else if(form->type & DT_MASKS_PATH)
    {
      dt_masks_point_path_t *ptbuf = (dt_masks_point_path_t *)malloc(nb_points*sizeof(dt_masks_point_path_t));
      memcpy(ptbuf, sqlite3_column_blob(stmt, 5), nb_points*sizeof(dt_masks_point_path_t));
      for (int i=0; i<nb_points; i++)
        form->points = g_list_append(form->points,ptbuf+i);
    }
    else if(form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *ptbuf = (dt_masks_point_group_t *)malloc(nb_points*sizeof(dt_masks_point_group_t));
      memcpy(ptbuf, sqlite3_column_blob(stmt, 5), nb_points*sizeof(dt_masks_point_group_t));
      for (int i=0; i<nb_points; i++)
        form->points = g_list_append(form->points,ptbuf+i);
    }

    //and we can add the form to the list
    dev->forms = g_list_append(dev->forms,form);
  }

  sqlite3_finalize (stmt);
  dt_dev_masks_list_change(dev);
}

void dt_masks_write_form(dt_masks_form_t *form, dt_develop_t *dev)
{
  //we first erase all masks for the image present in the db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1 and formid = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);

  //and we write the form
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into mask (imgid, formid, form, name, version, points, points_count,source) values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form->type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, form->name, strlen(form->name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, form->source, 2*sizeof(float), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, form->version);
  if (form->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, circle, sizeof(dt_masks_point_circle_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, 1);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
  }
  else if (form->type & DT_MASKS_PATH)
  {
    int nb = g_list_length(form->points);
    dt_masks_point_path_t *ptbuf = (dt_masks_point_path_t *)malloc(nb*sizeof(dt_masks_point_path_t));
    GList *points = g_list_first(form->points);
    int pos=0;
    while(points)
    {
      dt_masks_point_path_t *pt = (dt_masks_point_path_t *)points->data;
      ptbuf[pos++] = *pt;
      points = g_list_next(points);
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb*sizeof(dt_masks_point_path_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
    free(ptbuf);
  }
  else if (form->type & DT_MASKS_GROUP)
  {
    int nb = g_list_length(form->points);
    dt_masks_point_group_t *ptbuf = (dt_masks_point_group_t *)malloc(nb*sizeof(dt_masks_point_group_t));
    GList *points = g_list_first(form->points);
    int pos=0;
    while(points)
    {
      dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
      ptbuf[pos++] = *pt;
      points = g_list_next(points);
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb*sizeof(dt_masks_point_group_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
    free(ptbuf);
  }
}

void dt_masks_write_forms(dt_develop_t *dev)
{
  //we first erase all masks for the image present in the db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);

  //and now we write each forms
  GList *forms = g_list_first(dev->forms);
  while (forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *) forms->data;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into mask (imgid, formid, form, name, version, points, points_count,source) values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form->type);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, form->name, strlen(form->name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, form->source, 2*sizeof(float), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, form->version);
    if (form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, circle, sizeof(dt_masks_point_circle_t), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, 1);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
    else if (form->type & DT_MASKS_PATH)
    {
      int nb = g_list_length(form->points);
      dt_masks_point_path_t *ptbuf = (dt_masks_point_path_t *)malloc(nb*sizeof(dt_masks_point_path_t));
      GList *points = g_list_first(form->points);
      int pos=0;
      while(points)
      {
        dt_masks_point_path_t *pt = (dt_masks_point_path_t *)points->data;
        ptbuf[pos++] = *pt;
        points = g_list_next(points);
      }
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb*sizeof(dt_masks_point_path_t), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
      free(ptbuf);
    }
    else if (form->type & DT_MASKS_GROUP)
    {
      int nb = g_list_length(form->points);
      dt_masks_point_group_t *ptbuf = (dt_masks_point_group_t *)malloc(nb*sizeof(dt_masks_point_group_t));
      GList *points = g_list_first(form->points);
      int pos=0;
      while(points)
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
        ptbuf[pos++] = *pt;
        points = g_list_next(points);
      }
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb*sizeof(dt_masks_point_group_t), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
      free(ptbuf);
    }
    forms = g_list_next(forms);
  }
}

void dt_masks_free_form(dt_masks_form_t *form)
{
  if (!form) return;
  g_list_free(form->points);
  free(form);
  form = NULL;
}

int dt_masks_events_mouse_moved (struct dt_iop_module_t *module, double x, double y, int which)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;

  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  int rep = 0;
  if (form->type & DT_MASKS_CIRCLE) rep = dt_circle_events_mouse_moved(module,pzx,pzy,which,form,0,gui,0);
  else if (form->type & DT_MASKS_PATH) rep = dt_path_events_mouse_moved(module,pzx,pzy,which,form,0,gui,0);
  else if (form->type & DT_MASKS_GROUP) rep = dt_group_events_mouse_moved(module,pzx,pzy,which,form,gui);

  if (gui) _set_hinter_message(gui);

  return rep;
}
int dt_masks_events_button_released (struct dt_iop_module_t *module, double x, double y, int which, uint32_t state)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if (form->type & DT_MASKS_CIRCLE) return dt_circle_events_button_released(module,pzx,pzy,which,state,form,0,gui,0);
  else if (form->type & DT_MASKS_PATH) return dt_path_events_button_released(module,pzx,pzy,which,state,form,0,gui,0);
  else if (form->type & DT_MASKS_GROUP) return dt_group_events_button_released(module,pzx,pzy,which,state,form,gui);

  return 0;
}

int dt_masks_events_button_pressed (struct dt_iop_module_t *module, double x, double y, int which, int type, uint32_t state)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if (form->type & DT_MASKS_CIRCLE) return dt_circle_events_button_pressed(module,pzx,pzy,which,type,state,form,0,gui,0);
  else if (form->type & DT_MASKS_PATH) return dt_path_events_button_pressed(module,pzx,pzy,which,type,state,form,0,gui,0);
  else if (form->type & DT_MASKS_GROUP) return dt_group_events_button_pressed(module,pzx,pzy,which,type,state,form,gui);

  return 0;
}

int dt_masks_events_mouse_scrolled (struct dt_iop_module_t *module, double x, double y, int up, uint32_t state)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if (form->type & DT_MASKS_CIRCLE) return dt_circle_events_mouse_scrolled(module,pzx,pzy,up,state,form,0,gui,0);
  else if (form->type & DT_MASKS_PATH) return dt_path_events_mouse_scrolled(module,pzx,pzy,up,state,form,0,gui,0);
  else if (form->type & DT_MASKS_GROUP) return dt_group_events_mouse_scrolled(module,pzx,pzy,up,state,form,gui);

  return 0;
}
void dt_masks_events_post_expose (struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if (!gui) return;
  if (!form) return;
  //if it's a spot in creation, nothing to draw
  if ((form->type & DT_MASKS_CIRCLE) && gui->creation) return;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  if (wd < 1.0 || ht < 1.0) return;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  float zoom_x, zoom_y;
  int32_t zoom, closeup;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_set_source_rgb(cr, .3, .3, .3);

  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);

  //we update the form if needed
  dt_masks_gui_form_test_create(form,gui);

  //draw form
  if (form->type & DT_MASKS_CIRCLE) dt_circle_events_post_expose(cr,zoom_scale,gui,0);
  else if (form->type & DT_MASKS_PATH) dt_path_events_post_expose(cr,zoom_scale,gui,0,g_list_length(form->points));
  else if (form->type & DT_MASKS_GROUP) dt_group_events_post_expose(cr,zoom_scale,form,gui);
}

void dt_masks_init_formgui(dt_develop_t *dev)
{
  if (dev->form_gui->points) g_list_free(dev->form_gui->points);
  dev->form_gui->points = NULL;
  dev->form_gui->pipe_hash = dev->form_gui->formid = 0;
  dev->form_gui->posx = dev->form_gui->posy = dev->form_gui->dx = dev->form_gui->dy = 0.0f;
  dev->form_gui->scrollx = dev->form_gui->scrolly = 0.0f;
  dev->form_gui->form_selected = dev->form_gui->border_selected = dev->form_gui->form_dragging = FALSE;
  dev->form_gui->source_selected = dev->form_gui->source_dragging = FALSE;
  dev->form_gui->point_border_selected = dev->form_gui->seg_selected = dev->form_gui->point_selected = dev->form_gui->feather_selected = -1;
  dev->form_gui->point_border_dragging = dev->form_gui->seg_dragging = dev->form_gui->feather_dragging = dev->form_gui->point_dragging = -1;
  dev->form_gui->creation_closing_form = dev->form_gui->creation = FALSE;
  dev->form_gui->creation_module = NULL;
  dev->form_gui->point_edited = -1;

  dev->form_gui->group_edited = -1;
  dev->form_gui->group_selected = -1;
}

void dt_masks_change_form_gui(dt_masks_form_t *newform)
{
  dt_masks_init_formgui(darktable.develop);
  darktable.develop->form_visible = newform;
}

void dt_masks_reset_form_gui(void)
{
  darktable.develop->form_visible = NULL;
  dt_masks_init_formgui(darktable.develop);
  dt_iop_module_t *m = darktable.develop->gui_module;
  if ((m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)m->blend_data;
    bd->masks_shown = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), 0);
  }
}

void dt_masks_reset_show_masks_icons(void)
{
  GList *modules = g_list_first(darktable.develop->iop);
  while (modules)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if ((m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)m->blend_data;
      bd->masks_shown = 0;
      GTK_TOGGLE_BUTTON(bd->masks_edit)->active = 0;
      gtk_widget_queue_draw(bd->masks_edit);
    }
    modules = g_list_next(modules);
  }
}


void dt_masks_set_edit_mode(struct dt_iop_module_t *module,gboolean value)
{
  if (!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  dt_masks_form_t *grp = NULL;
  dt_masks_form_t *form = dt_masks_get_from_id(module->dev,module->blend_params->mask_id);
  if (value && form)
  {
    grp = dt_masks_create(DT_MASKS_GROUP);
    grp->formid = 0;
    dt_masks_group_ungroup(grp,form);
  }
  if (!(module->flags()&IOP_FLAGS_NO_MASKS))
  {
    bd->masks_shown = value;
  }
  dt_masks_change_form_gui(grp);
  if (value && form) dt_dev_masks_selection_change(darktable.develop,form->formid,FALSE);
  else dt_dev_masks_selection_change(darktable.develop,0,FALSE);
  dt_control_queue_redraw_center();
}

void dt_masks_iop_edit_toggle_callback(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  if (!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if (module->blend_params->mask_id==0)
  {
    bd->masks_shown = 0;
    return;
  }

  //reset the gui
  dt_masks_set_edit_mode(module,!bd->masks_shown);
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  //we drop all the forms in the iop
  //NOTE : maybe a little bit too definitive ? just add a state "not used" ?
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,module->blend_params->mask_id);
  if (grp) dt_masks_form_remove(module,NULL,grp);
  module->blend_params->mask_id = 0;

  //and we update the iop
  dt_masks_set_edit_mode(module,FALSE);
  dt_masks_iop_update(module);

  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_dev_masks_list_change(darktable.develop);
}
static void _menu_add_circle(struct dt_iop_module_t *module)
{
  //we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  //we create the new form
  dt_masks_form_t *spot = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_change_form_gui(spot);

  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}
static void _menu_add_path(struct dt_iop_module_t *module)
{
  //we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  //we create the new form
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}
static void _menu_add_exist(dt_iop_module_t *module, int formid)
{
  if (!module) return;

  //is there already a masks group for this module ?
  int grpid = module->blend_params->mask_id;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,grpid);
  if (!grp)
  {
    //we create a new group
    grp = dt_masks_create(DT_MASKS_GROUP);
    snprintf(grp->name,128,"grp %s",module->name());
    _check_id(grp);
    darktable.develop->forms = g_list_append(darktable.develop->forms,grp);
    module->blend_params->mask_id = grpid = grp->formid;
  }
  //we add the form in this group
  dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
  grpt->formid = formid;
  grpt->parentid = grpid;
  grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  if (g_list_length(grp->points)>0) grpt->state |= DT_MASKS_STATE_UNION;
  grpt->opacity = 1.0f;
  grp->points = g_list_append(grp->points,grpt);
  //we save the group
  dt_masks_write_form(grp,darktable.develop);

  //and we ensure that we are in edit mode
  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_masks_iop_update(module);
  dt_dev_masks_list_change(darktable.develop);
  dt_masks_set_edit_mode(module,TRUE);
}
static void _menu_use_same_as(dt_iop_module_t *module, dt_iop_module_t *src)
{
  if (!module || !src) return;

  //we get the source group
  int srcid = src->blend_params->mask_id;
  dt_masks_form_t *src_grp = dt_masks_get_from_id(darktable.develop,srcid);
  if (!src_grp || src_grp->type!=DT_MASKS_GROUP) return;

  //is there already a masks group for this module ?
  int grpid = module->blend_params->mask_id;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,grpid);
  if (!grp)
  {
    //we create a new group
    grp = dt_masks_create(DT_MASKS_GROUP);
    snprintf(grp->name,128,"grp %s",module->name());
    _check_id(grp);
    darktable.develop->forms = g_list_append(darktable.develop->forms,grp);
    module->blend_params->mask_id = grpid = grp->formid;
  }
  //we copy the src group in this group
  GList *points = g_list_first(src_grp->points);
  while (points)
  {
    dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    grpt->formid = pt->formid;
    grpt->parentid = grpid;
    grpt->state = pt->state;
    grpt->opacity = pt->opacity;
    grp->points = g_list_append(grp->points,grpt);
    points = g_list_next(points);
  }

  //we save the group
  dt_masks_write_form(grp,darktable.develop);

  //and we ensure that we are in edit mode
  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_masks_iop_update(module);
  dt_dev_masks_list_change(darktable.develop);
  dt_masks_set_edit_mode(module,TRUE);
}

void dt_masks_iop_combo_populate(struct dt_iop_module_t **m)
{
  //we ensure that the module has focus
  dt_iop_module_t *module = *m;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  //we determine a higher approx of the entry number
  int nbe = 5+g_list_length(darktable.develop->forms)+g_list_length(darktable.develop->iop);
  free(bd->masks_combo_ids);
  bd->masks_combo_ids = malloc(nbe*sizeof(int));

  int *cids = bd->masks_combo_ids;
  GtkWidget *combo = bd->masks_combo;

  //we remove all the combo entries except the first one
  while (dt_bauhaus_combobox_length(combo) > 1)
  {
    dt_bauhaus_combobox_remove_at(combo,1);
  }

  int pos = 0;
  cids[pos++] = 0;  //nothing to do for the first entry (already here)


  //add existing shapes
  GList *forms = g_list_first(darktable.develop->forms);
  int nb=0;
  while (forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if ((form->type & DT_MASKS_CLONE) || form->formid == module->blend_params->mask_id)
    {
      forms = g_list_next(forms);
      continue;
    }
    char str[256] = "";
    strcat(str,form->name);
    strcat(str,"   ");
    int used = 0;

    //we search were this form is used in the current module
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,module->blend_params->mask_id);
    if (grp && (grp->type & DT_MASKS_GROUP))
    {
      GList *pts = g_list_first(grp->points);
      while(pts)
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
        if (pt->formid == form->formid)
        {
          used = 1;
          break;
        }
        pts = g_list_next(pts);
      }
    }
    if (!used)
    {
      if (nb==0)
      {
        char str2[256] = "<";
        strcat(str2,_("add existing shape"));
        dt_bauhaus_combobox_add(combo,str2);
        cids[pos++] = 0;  //nothing to do
      }
      dt_bauhaus_combobox_add(combo,str);
      cids[pos++] = form->formid;
      nb++;
    }

    forms = g_list_next(forms);
  }

  //masks from other iops
  GList *modules = g_list_first(darktable.develop->iop);
  nb = 0;
  int pos2 = 1;
  while (modules)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if ((m!=module) && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,m->blend_params->mask_id);
      if (grp)
      {
        if (nb==0)
        {
          char str2[256] = "<";
          strcat(str2,_("use same shapes as"));
          dt_bauhaus_combobox_add(combo,str2);
          cids[pos++] = 0;  //nothing to do
        }
        char str[256] = "";
        strcat(str,m->name());
        strcat(str," ");
        strcat(str,m->multi_name);
        strcat(str,"   ");
        dt_bauhaus_combobox_add(combo,str);
        cids[pos++] = -1*pos2;
        nb++;
      }
    }
    pos2++;
    modules = g_list_next(modules);
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  //we get the corresponding value
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  int sel = dt_bauhaus_combobox_get(bd->masks_combo);
  if (sel==0) return;
  if (sel > 0)
  {
    int val = bd->masks_combo_ids[sel];
    if (val == -1000000)
    {
      //delete all masks
      _menu_no_masks(module);
    }
    else if (val == -2000001)
    {
      //add a circle shape
      _menu_add_circle(module);
    }
    else if (val == -2000002)
    {
      //add a path shape
      _menu_add_path(module);
    }
    else if (val < 0)
    {
      //use same shapes as another iop
      val = -1*val - 1;
      if (val < g_list_length(module->dev->iop))
      {
        dt_iop_module_t *m = (dt_iop_module_t *)g_list_nth_data(module->dev->iop,val);
        _menu_use_same_as(module,m);
      }
    }
    else if (val > 0)
    {
      //add an existing shape
      _menu_add_exist(module,val);
    }
  }
  //we update the combo line
  dt_masks_iop_update(module);
}

void dt_masks_iop_update(struct dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;
  dt_iop_gui_update(module);
  if (!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || (module->flags() & IOP_FLAGS_NO_MASKS) || !bd || !bd->blend_inited) return;

  /* update masks state */
  int nb = 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,module->blend_params->mask_id);
  if (grp && (grp->type&DT_MASKS_GROUP)) nb = g_list_length(grp->points);
  dt_bauhaus_combobox_clear(bd->masks_combo);
  free(bd->masks_combo_ids);
  bd->masks_combo_ids = NULL;
  if (nb>0)
  {
    char txt[512];
    snprintf(txt,512,ngettext("%d shape used", "%d shapes used", nb), nb);
    dt_bauhaus_combobox_add(bd->masks_combo,txt);
  }
  else dt_bauhaus_combobox_add(bd->masks_combo,_("no mask used"));

  dt_bauhaus_combobox_set(bd->masks_combo,0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown);
}

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form)
{
  int id = form->formid;
  if (!form) return;
  if (grp && !(grp->type & DT_MASKS_GROUP)) return;

  if (!(form->type & DT_MASKS_CLONE) && grp)
  {
    //we try to remove the form from the masks group
    int ok = 0;
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if (grpt->formid == id)
      {
        ok = 1;
        grp->points = g_list_remove(grp->points,grpt);
        break;
      }
      forms = g_list_next(forms);
    }
    if (ok) dt_masks_write_form(grp,darktable.develop);
    if (ok && module)
    {
      dt_masks_iop_update(module);
      dt_masks_update_image(darktable.develop);
    }
    if (ok && g_list_length(grp->points)==0) dt_masks_form_remove(module,NULL,grp);
    return;
  }

  //if we are here that mean we have to permanently delete this form
  //we drop the form from all modules
  GList *iops = g_list_first(darktable.develop->iop);
  while(iops)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
    if (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      int ok = 0;
      //is the form the base group of the iop ?
      if (id == m->blend_params->mask_id)
      {
        m->blend_params->mask_id = 0;
        dt_masks_iop_update(m);
        dt_dev_add_history_item(darktable.develop, m, TRUE);
      }
      else
      {
        dt_masks_form_t *iopgrp = dt_masks_get_from_id(darktable.develop,m->blend_params->mask_id);
        if (iopgrp && (iopgrp->type & DT_MASKS_GROUP))
        {
          GList *forms = g_list_first(iopgrp->points);
          while(forms)
          {
            dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
            if (grpt->formid == id)
            {
              ok = 1;
              iopgrp->points = g_list_remove(iopgrp->points,grpt);
              forms = g_list_first(iopgrp->points);
              continue;
            }
            forms = g_list_next(forms);
          }
          if (ok)
          {
            dt_masks_write_form(iopgrp,darktable.develop);
            dt_masks_iop_update(m);
            dt_masks_update_image(darktable.develop);
            if (g_list_length(iopgrp->points)==0) dt_masks_form_remove(m,NULL,iopgrp);
          }
        }
      }
    }
    iops = g_list_next(iops);
  }
  //we drop the form from the general list
  GList *forms = g_list_first(darktable.develop->forms);
  while (forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if (form->formid == id)
    {
      darktable.develop->forms = g_list_remove(darktable.develop->forms,form);
      dt_masks_write_forms(darktable.develop);
      break;
    }
    forms = g_list_next(forms);
  }
}

void dt_masks_form_change_opacity(dt_masks_form_t *form, int parentid, int up)
{
  if (!form) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop,parentid);
  if (!grp || !(grp->type & DT_MASKS_GROUP)) return;

  //we first need to test if the opacity can be set to the form
  if (form->type & DT_MASKS_GROUP) return;
  int id = form->formid;
  float amount = 0.05f;
  if (!up) amount = -amount;

  //so we change the value inside the group
  GList *fpts = g_list_first(grp->points);
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *) fpts->data;
    if (fpt->formid == id)
    {
      float nv = fpt->opacity + amount;
      if (nv<=1.0f && nv>=0.0f)
      {
        fpt->opacity = nv;
        dt_masks_write_form(grp,darktable.develop);
        dt_masks_update_image(darktable.develop);
        dt_dev_masks_list_update(darktable.develop);
      }
      break;
    }
    fpts = g_list_next(fpts);
  }
}

void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up)
{
  if (!grp || !(grp->type & DT_MASKS_GROUP)) return;

  //we search the form in the group
  dt_masks_point_group_t *grpt = NULL;
  int pos=0;
  GList *fpts = g_list_first(grp->points);
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *) fpts->data;
    if (fpt->formid == formid)
    {
      grpt = fpt;
      break;
    }
    pos++;
    fpts = g_list_next(fpts);
  }

  //we remove the form and readd it
  if (grpt)
  {
    if (up && pos==0) return;
    if (!up && pos == g_list_length(grp->points)-1) return;

    grp->points = g_list_remove(grp->points,grpt);
    if (up) pos -= 1;
    else pos += 1;
    grp->points = g_list_insert(grp->points,grpt,pos);
    dt_masks_write_form(grp,darktable.develop);
  }
}

void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp)
{
  if (!grp || !dest_grp) return;
  if (!(grp->type&DT_MASKS_GROUP) || !(dest_grp->type&DT_MASKS_GROUP)) return;

  GList *forms = g_list_first(grp->points);
  while(forms)
  {
    dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop,grpt->formid);
    if(form)
    {
      if (form->type & DT_MASKS_GROUP)
      {
        dt_masks_group_ungroup(dest_grp,form);
      }
      else
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *) malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = grpt->formid;
        fpt->parentid = grpt->parentid;
        fpt->state = grpt->state;
        fpt->opacity = grpt->opacity;
        dest_grp->points = g_list_append(dest_grp->points,fpt);
      }
    }
    forms = g_list_next(forms);
  }
}

int dt_masks_group_get_hash_buffer_length(dt_masks_form_t *form)
{
  if (!form) return 0;
  int pos = 0;
  //basic infos
  pos += sizeof(dt_masks_type_t);
  pos += sizeof(int);
  pos += sizeof(int);
  pos += 2*sizeof(float);

  GList *forms = g_list_first(form->points);
  while(forms)
  {
    if (form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop,grpt->formid);
      if (f)
      {
        //state & opacity
        pos += sizeof(int);
        pos += sizeof(float);
        //the form itself
        pos += dt_masks_group_get_hash_buffer_length(f);
      }
    }
    else if (form->type & DT_MASKS_CIRCLE)
    {
      pos += sizeof(dt_masks_point_circle_t);
    }
    else if (form->type & DT_MASKS_PATH)
    {
      pos += sizeof(dt_masks_point_path_t);
    }
    forms = g_list_next(forms);
  }
  return pos;
}

char *dt_masks_group_get_hash_buffer(dt_masks_form_t *form, char *str)
{
  if (!form) return str;
  int pos = 0;
  //basic infos
  memcpy(str+pos, &form->type, sizeof(dt_masks_type_t));
  pos += sizeof(dt_masks_type_t);
  memcpy(str+pos, &form->formid, sizeof(int));
  pos += sizeof(int);
  memcpy(str+pos, &form->version, sizeof(int));
  pos += sizeof(int);
  memcpy(str+pos, &form->source, 2*sizeof(float));
  pos += 2*sizeof(float);

  GList *forms = g_list_first(form->points);
  while(forms)
  {
    if (form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop,grpt->formid);
      if (f)
      {
        //state & opacity
        memcpy(str+pos, &grpt->state, sizeof(int));
        pos += sizeof(int);
        memcpy(str+pos, &grpt->opacity, sizeof(float));
        pos += sizeof(float);
        //the form itself
        str = dt_masks_group_get_hash_buffer(f,str+pos)-pos;
      }
    }
    else if (form->type & DT_MASKS_CIRCLE)
    {
      memcpy(str+pos, forms->data, sizeof(dt_masks_point_circle_t));
      pos += sizeof(dt_masks_point_circle_t);
    }
    else if (form->type & DT_MASKS_PATH)
    {
      memcpy(str+pos, forms->data, sizeof(dt_masks_point_path_t));
      pos += sizeof(dt_masks_point_path_t);
    }
    forms = g_list_next(forms);
  }
  return str+pos;
}

void dt_masks_update_image(dt_develop_t *dev)
{
  /* invalidate image data*/
  //dt_similarity_image_dirty(dev->image_storage.id);

  // invalidate buffers and force redraw of darkroom
  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate_all(dev);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dev->image_storage.id);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
