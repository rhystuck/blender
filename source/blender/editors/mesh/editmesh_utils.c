/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2004 by Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_alloca.h"
#include "BLI_buffer.h"
#include "BLI_kdtree.h"
#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_DerivedMesh.h"
#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_editmesh_bvh.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_report.h"

#include "DEG_depsgraph.h"

#include "BKE_object.h" /* XXX. only for EDBM_mesh_load(). */

#include "WM_api.h"
#include "WM_types.h"

#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_transform_snap_object_context.h"
#include "ED_uvedit.h"
#include "ED_view3d.h"

#include "mesh_intern.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name Redo API
 * \{ */

/* Mesh backup implementation.
 * This would greatly benefit from some sort of binary diffing
 * just as the undo stack would.
 * So leaving this as an interface for further work */

BMBackup EDBM_redo_state_store(BMEditMesh *em)
{
  BMBackup backup;
  backup.bmcopy = BM_mesh_copy(em->bm);
  return backup;
}

void EDBM_redo_state_restore(BMBackup backup, BMEditMesh *em, int recalctess)
{
  BMesh *tmpbm;
  if (!em || !backup.bmcopy) {
    return;
  }

  BM_mesh_data_free(em->bm);
  tmpbm = BM_mesh_copy(backup.bmcopy);
  *em->bm = *tmpbm;
  MEM_freeN(tmpbm);
  tmpbm = NULL;

  if (recalctess) {
    BKE_editmesh_looptri_calc(em);
  }
}

void EDBM_redo_state_free(BMBackup *backup, BMEditMesh *em, int recalctess)
{
  if (em && backup->bmcopy) {
    BM_mesh_data_free(em->bm);
    *em->bm = *backup->bmcopy;
  }
  else if (backup->bmcopy) {
    BM_mesh_data_free(backup->bmcopy);
  }

  if (backup->bmcopy) {
    MEM_freeN(backup->bmcopy);
  }
  backup->bmcopy = NULL;

  if (recalctess && em) {
    BKE_editmesh_looptri_calc(em);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BMesh Operator (BMO) API Wrapper
 * \{ */

bool EDBM_op_init(BMEditMesh *em, BMOperator *bmop, wmOperator *op, const char *fmt, ...)
{
  BMesh *bm = em->bm;
  va_list list;

  va_start(list, fmt);

  if (!BMO_op_vinitf(bm, bmop, BMO_FLAG_DEFAULTS, fmt, list)) {
    BKE_reportf(op->reports, RPT_ERROR, "Parse error in %s", __func__);
    va_end(list);
    return false;
  }

  if (!em->emcopy) {
    em->emcopy = BKE_editmesh_copy(em);
  }
  em->emcopyusers++;

  va_end(list);

  return true;
}

/* returns 0 on error, 1 on success.  executes and finishes a bmesh operator */
bool EDBM_op_finish(BMEditMesh *em, BMOperator *bmop, wmOperator *op, const bool do_report)
{
  const char *errmsg;

  BMO_op_finish(em->bm, bmop);

  if (BMO_error_get(em->bm, &errmsg, NULL)) {
    BMEditMesh *emcopy = em->emcopy;

    if (do_report) {
      BKE_report(op->reports, RPT_ERROR, errmsg);
    }

    EDBM_mesh_free(em);
    *em = *emcopy;

    MEM_freeN(emcopy);
    em->emcopyusers = 0;
    em->emcopy = NULL;

    /**
     * Note, we could pass in the mesh, however this is an exceptional case, allow a slow lookup.
     *
     * This is needed because the COW mesh makes a full copy of the #BMEditMesh
     * instead of sharing the pointer, tagging since this has been freed above,
     * the #BMEditMesh.emcopy needs to be flushed to the COW edit-mesh, see T55457.
     */
    {
      Main *bmain = G_MAIN;
      for (Mesh *mesh = bmain->meshes.first; mesh; mesh = mesh->id.next) {
        if (mesh->edit_mesh == em) {
          DEG_id_tag_update(&mesh->id, ID_RECALC_COPY_ON_WRITE);
          break;
        }
      }
    }

    /* when copying, tessellation isn't to for faster copying,
     * but means we need to re-tessellate here */
    if (em->looptris == NULL) {
      BKE_editmesh_looptri_calc(em);
    }

    return false;
  }

  em->emcopyusers--;
  if (em->emcopyusers < 0) {
    printf("warning: em->emcopyusers was less than zero.\n");
  }

  if (em->emcopyusers <= 0) {
    BKE_editmesh_free(em->emcopy);
    MEM_freeN(em->emcopy);
    em->emcopy = NULL;
  }

  return true;
}

bool EDBM_op_callf(BMEditMesh *em, wmOperator *op, const char *fmt, ...)
{
  BMesh *bm = em->bm;
  BMOperator bmop;
  va_list list;

  va_start(list, fmt);

  if (!BMO_op_vinitf(bm, &bmop, BMO_FLAG_DEFAULTS, fmt, list)) {
    BKE_reportf(op->reports, RPT_ERROR, "Parse error in %s", __func__);
    va_end(list);
    return false;
  }

  if (!em->emcopy) {
    em->emcopy = BKE_editmesh_copy(em);
  }
  em->emcopyusers++;

  BMO_op_exec(bm, &bmop);

  va_end(list);
  return EDBM_op_finish(em, &bmop, op, true);
}

bool EDBM_op_call_and_selectf(BMEditMesh *em,
                              wmOperator *op,
                              const char *select_slot_out,
                              const bool select_extend,
                              const char *fmt,
                              ...)
{
  BMOpSlot *slot_select_out;
  BMesh *bm = em->bm;
  BMOperator bmop;
  va_list list;
  char hflag;

  va_start(list, fmt);

  if (!BMO_op_vinitf(bm, &bmop, BMO_FLAG_DEFAULTS, fmt, list)) {
    BKE_reportf(op->reports, RPT_ERROR, "Parse error in %s", __func__);
    va_end(list);
    return false;
  }

  if (!em->emcopy) {
    em->emcopy = BKE_editmesh_copy(em);
  }
  em->emcopyusers++;

  BMO_op_exec(bm, &bmop);

  slot_select_out = BMO_slot_get(bmop.slots_out, select_slot_out);
  hflag = slot_select_out->slot_subtype.elem & BM_ALL_NOLOOP;
  BLI_assert(hflag != 0);

  if (select_extend == false) {
    BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  }

  BMO_slot_buffer_hflag_enable(
      em->bm, bmop.slots_out, select_slot_out, hflag, BM_ELEM_SELECT, true);

  va_end(list);
  return EDBM_op_finish(em, &bmop, op, true);
}

bool EDBM_op_call_silentf(BMEditMesh *em, const char *fmt, ...)
{
  BMesh *bm = em->bm;
  BMOperator bmop;
  va_list list;

  va_start(list, fmt);

  if (!BMO_op_vinitf(bm, &bmop, BMO_FLAG_DEFAULTS, fmt, list)) {
    va_end(list);
    return false;
  }

  if (!em->emcopy) {
    em->emcopy = BKE_editmesh_copy(em);
  }
  em->emcopyusers++;

  BMO_op_exec(bm, &bmop);

  va_end(list);
  return EDBM_op_finish(em, &bmop, NULL, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit BMesh API
 *
 * Make/Clear/Free functions.
 * \{ */

void EDBM_mesh_make(Object *ob, const int select_mode, const bool add_key_index)
{
  Mesh *me = ob->data;
  BMesh *bm;

  bm = BKE_mesh_to_bmesh(me,
                         ob,
                         add_key_index,
                         &((struct BMeshCreateParams){
                             .use_toolflags = true,
                         }));

  if (me->edit_mesh) {
    /* this happens when switching shape keys */
    EDBM_mesh_free(me->edit_mesh);
    MEM_freeN(me->edit_mesh);
  }

  /* currently executing operators re-tessellates, so we can avoid doing here
   * but at some point it may need to be added back. */
#if 0
  me->edit_mesh = BKE_editmesh_create(bm, true);
#else
  me->edit_mesh = BKE_editmesh_create(bm, false);
#endif

  me->edit_mesh->selectmode = me->edit_mesh->bm->selectmode = select_mode;
  me->edit_mesh->mat_nr = (ob->actcol > 0) ? ob->actcol - 1 : 0;

  /* we need to flush selection because the mode may have changed from when last in editmode */
  EDBM_selectmode_flush(me->edit_mesh);
}

/**
 * \warning This can invalidate the #Mesh runtime cache of other objects (for linked duplicates).
 * Most callers should run #DEG_id_tag_update on \a ob->data, see: T46738, T46913
 */
void EDBM_mesh_load_ex(Main *bmain, Object *ob, bool free_data)
{
  Mesh *me = ob->data;
  BMesh *bm = me->edit_mesh->bm;

  /* Workaround for T42360, 'ob->shapenr' should be 1 in this case.
   * however this isn't synchronized between objects at the moment. */
  if (UNLIKELY((ob->shapenr == 0) && (me->key && !BLI_listbase_is_empty(&me->key->block)))) {
    bm->shapenr = 1;
  }

  BM_mesh_bm_to_me(bmain,
                   bm,
                   me,
                   (&(struct BMeshToMeshParams){
                       .calc_object_remap = true,
                       .update_shapekey_indices = !free_data,
                   }));

  /* Free derived mesh. usually this would happen through depsgraph but there
   * are exceptions like file save that will not cause this, and we want to
   * avoid ending up with an invalid derived mesh then.
   *
   * Do it for all objects which shares the same mesh datablock, since their
   * derived meshes might also be referencing data which was just freed,
   *
   * Annoying enough, but currently seems most efficient way to avoid access
   * of freed data on scene update, especially in cases when there are dependency
   * cycles.
   */
#if 0
  for (Object *other_object = bmain->objects.first; other_object != NULL; other_object = other_object->id.next) {
    if (other_object->data == ob->data) {
      BKE_object_free_derived_caches(other_object);
    }
  }
#endif
}

void EDBM_mesh_clear(BMEditMesh *em)
{
  /* clear bmesh */
  BM_mesh_clear(em->bm);

  /* free derived meshes */
  BKE_editmesh_free_derivedmesh(em);

  /* free tessellation data */
  em->tottri = 0;
  if (em->looptris) {
    MEM_freeN(em->looptris);
    em->looptris = NULL;
  }
}

void EDBM_mesh_load(Main *bmain, Object *ob)
{
  EDBM_mesh_load_ex(bmain, ob, true);
}

/**
 * Should only be called on the active editmesh, otherwise call #BKE_editmesh_free
 */
void EDBM_mesh_free(BMEditMesh *em)
{
  /* These tables aren't used yet, so it's not strictly necessary
   * to 'end' them but if someone tries to start using them,
   * having these in place will save a lot of pain. */
  ED_mesh_mirror_spatial_table_end(NULL);
  ED_mesh_mirror_topo_table_end(NULL);

  BKE_editmesh_free(em);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selection Utilities
 * \{ */

void EDBM_selectmode_to_scene(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);

  if (!em) {
    return;
  }

  scene->toolsettings->selectmode = em->selectmode;

  /* Request redraw of header buttons (to show new select mode) */
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
}

void EDBM_selectmode_flush_ex(BMEditMesh *em, const short selectmode)
{
  BM_mesh_select_mode_flush_ex(em->bm, selectmode);
}

void EDBM_selectmode_flush(BMEditMesh *em)
{
  EDBM_selectmode_flush_ex(em, em->selectmode);
}

void EDBM_deselect_flush(BMEditMesh *em)
{
  /* function below doesn't use. just do this to keep the values in sync */
  em->bm->selectmode = em->selectmode;
  BM_mesh_deselect_flush(em->bm);
}

void EDBM_select_flush(BMEditMesh *em)
{
  /* function below doesn't use. just do this to keep the values in sync */
  em->bm->selectmode = em->selectmode;
  BM_mesh_select_flush(em->bm);
}

void EDBM_select_more(BMEditMesh *em, const bool use_face_step)
{
  BMOperator bmop;
  const bool use_faces = (em->selectmode == SCE_SELECT_FACE);

  BMO_op_initf(em->bm,
               &bmop,
               BMO_FLAG_DEFAULTS,
               "region_extend geom=%hvef use_contract=%b use_faces=%b use_face_step=%b",
               BM_ELEM_SELECT,
               false,
               use_faces,
               use_face_step);
  BMO_op_exec(em->bm, &bmop);
  /* don't flush selection in edge/vertex mode  */
  BMO_slot_buffer_hflag_enable(
      em->bm, bmop.slots_out, "geom.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, use_faces ? true : false);
  BMO_op_finish(em->bm, &bmop);

  EDBM_selectmode_flush(em);
}

void EDBM_select_less(BMEditMesh *em, const bool use_face_step)
{
  BMOperator bmop;
  const bool use_faces = (em->selectmode == SCE_SELECT_FACE);

  BMO_op_initf(em->bm,
               &bmop,
               BMO_FLAG_DEFAULTS,
               "region_extend geom=%hvef use_contract=%b use_faces=%b use_face_step=%b",
               BM_ELEM_SELECT,
               true,
               use_faces,
               use_face_step);
  BMO_op_exec(em->bm, &bmop);
  /* don't flush selection in edge/vertex mode  */
  BMO_slot_buffer_hflag_disable(
      em->bm, bmop.slots_out, "geom.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, use_faces ? true : false);
  BMO_op_finish(em->bm, &bmop);

  EDBM_selectmode_flush(em);

  /* only needed for select less, ensure we don't have isolated elements remaining */
  BM_mesh_select_mode_clean(em->bm);
}

void EDBM_flag_disable_all(BMEditMesh *em, const char hflag)
{
  BM_mesh_elem_hflag_disable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, hflag, false);
}

void EDBM_flag_enable_all(BMEditMesh *em, const char hflag)
{
  BM_mesh_elem_hflag_enable_all(em->bm, BM_VERT | BM_EDGE | BM_FACE, hflag, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Vertex Map API
 * \{ */

/**
 * Return a new #UvVertMap from the edit-mesh.
 */
UvVertMap *BM_uv_vert_map_create(BMesh *bm, const bool use_select, const bool use_winding)
{
  BMVert *ev;
  BMFace *efa;
  BMLoop *l;
  BMIter iter, liter;
  /* vars from original func */
  UvVertMap *vmap;
  UvMapVert *buf;
  const MLoopUV *luv;
  uint a;
  int totverts, i, totuv, totfaces;
  const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);
  bool *winding = NULL;
  BLI_buffer_declare_static(vec2f, tf_uv_buf, BLI_BUFFER_NOP, BM_DEFAULT_NGON_STACK_SIZE);

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_FACE);

  totfaces = bm->totface;
  totverts = bm->totvert;
  totuv = 0;

  /* generate UvMapVert array */
  BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
    if ((use_select == false) || BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      totuv += efa->len;
    }
  }

  if (totuv == 0) {
    return NULL;
  }
  vmap = (UvVertMap *)MEM_callocN(sizeof(*vmap), "UvVertMap");
  if (!vmap) {
    return NULL;
  }

  vmap->vert = (UvMapVert **)MEM_callocN(sizeof(*vmap->vert) * totverts, "UvMapVert_pt");
  buf = vmap->buf = (UvMapVert *)MEM_callocN(sizeof(*vmap->buf) * totuv, "UvMapVert");
  if (use_winding) {
    winding = MEM_callocN(sizeof(*winding) * totfaces, "winding");
  }

  if (!vmap->vert || !vmap->buf) {
    BKE_mesh_uv_vert_map_free(vmap);
    return NULL;
  }

  BM_ITER_MESH_INDEX (efa, &iter, bm, BM_FACES_OF_MESH, a) {
    if ((use_select == false) || BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      float(*tf_uv)[2] = NULL;

      if (use_winding) {
        tf_uv = (float(*)[2])BLI_buffer_reinit_data(&tf_uv_buf, vec2f, efa->len);
      }

      BM_ITER_ELEM_INDEX (l, &liter, efa, BM_LOOPS_OF_FACE, i) {
        buf->loop_of_poly_index = i;
        buf->poly_index = a;
        buf->separate = 0;

        buf->next = vmap->vert[BM_elem_index_get(l->v)];
        vmap->vert[BM_elem_index_get(l->v)] = buf;
        buf++;

        if (use_winding) {
          luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
          copy_v2_v2(tf_uv[i], luv->uv);
        }
      }

      if (use_winding) {
        winding[a] = cross_poly_v2(tf_uv, efa->len) > 0;
      }
    }
  }

  /* sort individual uvs for each vert */
  BM_ITER_MESH_INDEX (ev, &iter, bm, BM_VERTS_OF_MESH, a) {
    UvMapVert *newvlist = NULL, *vlist = vmap->vert[a];
    UvMapVert *iterv, *v, *lastv, *next;
    const float *uv, *uv2;

    while (vlist) {
      v = vlist;
      vlist = vlist->next;
      v->next = newvlist;
      newvlist = v;

      efa = BM_face_at_index(bm, v->poly_index);

      l = BM_iter_at_index(bm, BM_LOOPS_OF_FACE, efa, v->loop_of_poly_index);
      luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
      uv = luv->uv;

      lastv = NULL;
      iterv = vlist;

      while (iterv) {
        next = iterv->next;
        efa = BM_face_at_index(bm, iterv->poly_index);
        l = BM_iter_at_index(bm, BM_LOOPS_OF_FACE, efa, iterv->loop_of_poly_index);
        luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
        uv2 = luv->uv;

        if (compare_v2v2(uv2, uv, STD_UV_CONNECT_LIMIT) &&
            (!use_winding || winding[iterv->poly_index] == winding[v->poly_index])) {
          if (lastv) {
            lastv->next = next;
          }
          else {
            vlist = next;
          }
          iterv->next = newvlist;
          newvlist = iterv;
        }
        else {
          lastv = iterv;
        }

        iterv = next;
      }

      newvlist->separate = 1;
    }

    vmap->vert[a] = newvlist;
  }

  if (use_winding) {
    MEM_freeN(winding);
  }

  BLI_buffer_free(&tf_uv_buf);

  return vmap;
}

UvMapVert *BM_uv_vert_map_at_index(UvVertMap *vmap, uint v)
{
  return vmap->vert[v];
}

/* A specialized vert map used by stitch operator */
UvElementMap *BM_uv_element_map_create(BMesh *bm,
                                       const Scene *scene,
                                       const bool face_selected,
                                       const bool uv_selected,
                                       const bool use_winding,
                                       const bool do_islands)
{
  BMVert *ev;
  BMFace *efa;
  BMLoop *l;
  BMIter iter, liter;
  /* vars from original func */
  UvElementMap *element_map;
  UvElement *buf;
  bool *winding = NULL;
  BLI_buffer_declare_static(vec2f, tf_uv_buf, BLI_BUFFER_NOP, BM_DEFAULT_NGON_STACK_SIZE);

  MLoopUV *luv;
  int totverts, totfaces, i, totuv, j;

  const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_FACE);

  totfaces = bm->totface;
  totverts = bm->totvert;
  totuv = 0;

  /* generate UvElement array */
  BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
    if (!face_selected || BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      if (!uv_selected) {
        totuv += efa->len;
      }
      else {
        BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
          if (uvedit_uv_select_test(scene, l, cd_loop_uv_offset)) {
            totuv++;
          }
        }
      }
    }
  }

  if (totuv == 0) {
    return NULL;
  }

  element_map = (UvElementMap *)MEM_callocN(sizeof(*element_map), "UvElementMap");
  element_map->totalUVs = totuv;
  element_map->vert = (UvElement **)MEM_callocN(sizeof(*element_map->vert) * totverts,
                                                "UvElementVerts");
  buf = element_map->buf = (UvElement *)MEM_callocN(sizeof(*element_map->buf) * totuv,
                                                    "UvElement");

  if (use_winding) {
    winding = MEM_mallocN(sizeof(*winding) * totfaces, "winding");
  }

  BM_ITER_MESH_INDEX (efa, &iter, bm, BM_FACES_OF_MESH, j) {

    if (use_winding) {
      winding[j] = false;
    }

    if (!face_selected || BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      float(*tf_uv)[2] = NULL;

      if (use_winding) {
        tf_uv = (float(*)[2])BLI_buffer_reinit_data(&tf_uv_buf, vec2f, efa->len);
      }

      BM_ITER_ELEM_INDEX (l, &liter, efa, BM_LOOPS_OF_FACE, i) {
        if (uv_selected && !uvedit_uv_select_test(scene, l, cd_loop_uv_offset)) {
          continue;
        }

        buf->l = l;
        buf->separate = 0;
        buf->island = INVALID_ISLAND;
        buf->loop_of_poly_index = i;

        buf->next = element_map->vert[BM_elem_index_get(l->v)];
        element_map->vert[BM_elem_index_get(l->v)] = buf;

        if (use_winding) {
          luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
          copy_v2_v2(tf_uv[i], luv->uv);
        }

        buf++;
      }

      if (use_winding) {
        winding[j] = cross_poly_v2(tf_uv, efa->len) > 0;
      }
    }
  }

  /* sort individual uvs for each vert */
  BM_ITER_MESH_INDEX (ev, &iter, bm, BM_VERTS_OF_MESH, i) {
    UvElement *newvlist = NULL, *vlist = element_map->vert[i];
    UvElement *iterv, *v, *lastv, *next;
    const float *uv, *uv2;
    bool uv_vert_sel, uv2_vert_sel;

    while (vlist) {
      v = vlist;
      vlist = vlist->next;
      v->next = newvlist;
      newvlist = v;

      l = v->l;
      luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
      uv = luv->uv;
      uv_vert_sel = uvedit_uv_select_test(scene, l, cd_loop_uv_offset);

      lastv = NULL;
      iterv = vlist;

      while (iterv) {
        next = iterv->next;

        l = iterv->l;
        luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
        uv2 = luv->uv;
        uv2_vert_sel = uvedit_uv_select_test(scene, l, cd_loop_uv_offset);

        /* Check if the uv loops share the same selection state (if not, they are not connected as
         * they have been ripped or other edit commands have separated them). */
        const bool connected = (uv_vert_sel == uv2_vert_sel) &&
                               compare_v2v2(uv2, uv, STD_UV_CONNECT_LIMIT);

        if (connected && (!use_winding || winding[BM_elem_index_get(iterv->l->f)] ==
                                              winding[BM_elem_index_get(v->l->f)])) {
          if (lastv) {
            lastv->next = next;
          }
          else {
            vlist = next;
          }
          iterv->next = newvlist;
          newvlist = iterv;
        }
        else {
          lastv = iterv;
        }

        iterv = next;
      }

      newvlist->separate = 1;
    }

    element_map->vert[i] = newvlist;
  }

  if (use_winding) {
    MEM_freeN(winding);
  }

  if (do_islands) {
    uint *map;
    BMFace **stack;
    int stacksize = 0;
    UvElement *islandbuf;
    /* island number for faces */
    int *island_number = NULL;

    int nislands = 0, islandbufsize = 0;

    /* map holds the map from current vmap->buf to the new, sorted map */
    map = MEM_mallocN(sizeof(*map) * totuv, "uvelement_remap");
    stack = MEM_mallocN(sizeof(*stack) * bm->totface, "uv_island_face_stack");
    islandbuf = MEM_callocN(sizeof(*islandbuf) * totuv, "uvelement_island_buffer");
    island_number = MEM_mallocN(sizeof(*island_number) * totfaces, "uv_island_number_face");
    copy_vn_i(island_number, totfaces, INVALID_ISLAND);

    /* at this point, every UvElement in vert points to a UvElement sharing the same vertex.
     * Now we should sort uv's in islands. */
    for (i = 0; i < totuv; i++) {
      if (element_map->buf[i].island == INVALID_ISLAND) {
        element_map->buf[i].island = nislands;
        stack[0] = element_map->buf[i].l->f;
        island_number[BM_elem_index_get(stack[0])] = nislands;
        stacksize = 1;

        while (stacksize > 0) {
          efa = stack[--stacksize];

          BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
            if (uv_selected && !uvedit_uv_select_test(scene, l, cd_loop_uv_offset)) {
              continue;
            }

            UvElement *element, *initelement = element_map->vert[BM_elem_index_get(l->v)];

            for (element = initelement; element; element = element->next) {
              if (element->separate) {
                initelement = element;
              }

              if (element->l->f == efa) {
                /* found the uv corresponding to our face and vertex.
                 * Now fill it to the buffer */
                element->island = nislands;
                map[element - element_map->buf] = islandbufsize;
                islandbuf[islandbufsize].l = element->l;
                islandbuf[islandbufsize].separate = element->separate;
                islandbuf[islandbufsize].loop_of_poly_index = element->loop_of_poly_index;
                islandbuf[islandbufsize].island = nislands;
                islandbufsize++;

                for (element = initelement; element; element = element->next) {
                  if (element->separate && element != initelement) {
                    break;
                  }

                  if (island_number[BM_elem_index_get(element->l->f)] == INVALID_ISLAND) {
                    stack[stacksize++] = element->l->f;
                    island_number[BM_elem_index_get(element->l->f)] = nislands;
                  }
                }
                break;
              }
            }
          }
        }

        nislands++;
      }
    }

    MEM_freeN(island_number);

    /* remap */
    for (i = 0; i < bm->totvert; i++) {
      /* important since we may do selection only. Some of these may be NULL */
      if (element_map->vert[i]) {
        element_map->vert[i] = &islandbuf[map[element_map->vert[i] - element_map->buf]];
      }
    }

    element_map->islandIndices = MEM_callocN(sizeof(*element_map->islandIndices) * nislands,
                                             "UvElementMap_island_indices");
    j = 0;
    for (i = 0; i < totuv; i++) {
      UvElement *element = element_map->buf[i].next;
      if (element == NULL) {
        islandbuf[map[i]].next = NULL;
      }
      else {
        islandbuf[map[i]].next = &islandbuf[map[element - element_map->buf]];
      }

      if (islandbuf[i].island != j) {
        j++;
        element_map->islandIndices[j] = i;
      }
    }

    MEM_freeN(element_map->buf);

    element_map->buf = islandbuf;
    element_map->totalIslands = nislands;
    MEM_freeN(stack);
    MEM_freeN(map);
  }

  BLI_buffer_free(&tf_uv_buf);

  return element_map;
}

void BM_uv_vert_map_free(UvVertMap *vmap)
{
  if (vmap) {
    if (vmap->vert) {
      MEM_freeN(vmap->vert);
    }
    if (vmap->buf) {
      MEM_freeN(vmap->buf);
    }
    MEM_freeN(vmap);
  }
}

void BM_uv_element_map_free(UvElementMap *element_map)
{
  if (element_map) {
    if (element_map->vert) {
      MEM_freeN(element_map->vert);
    }
    if (element_map->buf) {
      MEM_freeN(element_map->buf);
    }
    if (element_map->islandIndices) {
      MEM_freeN(element_map->islandIndices);
    }
    MEM_freeN(element_map);
  }
}

UvElement *BM_uv_element_get(UvElementMap *map, BMFace *efa, BMLoop *l)
{
  for (UvElement *element = map->vert[BM_elem_index_get(l->v)]; element; element = element->next) {
    if (element->l->f == efa) {
      return element;
    }
  }

  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data Layer Checks
 * \{ */

/**
 * last_sel, use em->act_face otherwise get the last selected face in the editselections
 * at the moment, last_sel is mainly useful for making sure the space image doesn't flicker.
 */
BMFace *EDBM_uv_active_face_get(BMEditMesh *em, const bool sloppy, const bool selected)
{
  BMFace *efa = NULL;

  if (!EDBM_uv_check(em)) {
    return NULL;
  }

  efa = BM_mesh_active_face_get(em->bm, sloppy, selected);

  if (efa) {
    return efa;
  }

  return NULL;
}

/* can we edit UV's for this mesh?*/
bool EDBM_uv_check(BMEditMesh *em)
{
  /* some of these checks could be a touch overkill */
  return em && em->bm->totface && CustomData_has_layer(&em->bm->ldata, CD_MLOOPUV);
}

bool EDBM_vert_color_check(BMEditMesh *em)
{
  /* some of these checks could be a touch overkill */
  return em && em->bm->totface && CustomData_has_layer(&em->bm->ldata, CD_MLOOPCOL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mirror Cache API
 * \{ */

static BMVert *cache_mirr_intptr_as_bmvert(const intptr_t *index_lookup, int index)
{
  intptr_t eve_i = index_lookup[index];
  return (eve_i == -1) ? NULL : (BMVert *)eve_i;
}

/**
 * Mirror editing API, usage:
 *
 * \code{.c}
 * EDBM_verts_mirror_cache_begin(em, ...);
 *
 * BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
 *     v_mirror = EDBM_verts_mirror_get(em, v);
 *     e_mirror = EDBM_verts_mirror_get_edge(em, e);
 *     f_mirror = EDBM_verts_mirror_get_face(em, f);
 * }
 *
 * EDBM_verts_mirror_cache_end(em);
 * \endcode
 */

/* BM_SEARCH_MAXDIST is too big, copied from 2.6x MOC_THRESH, should become a
 * preference */
#define BM_SEARCH_MAXDIST_MIRR 0.00002f
#define BM_CD_LAYER_ID "__mirror_index"
/**
 * \param em: Editmesh.
 * \param use_self: Allow a vertex to point to its self (middle verts).
 * \param use_select: Restrict to selected verts.
 * \param respecthide: Skip hidden vertices.
 * \param use_topology: Use topology mirror.
 * \param maxdist: Distance for close point test.
 * \param r_index: Optional array to write into, as an alternative to a customdata layer
 * (length of total verts).
 */
void EDBM_verts_mirror_cache_begin_ex(BMEditMesh *em,
                                      const int axis,
                                      const bool use_self,
                                      const bool use_select,
                                      const bool respecthide,
                                      /* extra args */
                                      const bool use_topology,
                                      float maxdist,
                                      int *r_index)
{
  BMesh *bm = em->bm;
  BMIter iter;
  BMVert *v;
  int cd_vmirr_offset = 0;
  int i;
  const float maxdist_sq = square_f(maxdist);

  /* one or the other is used depending if topo is enabled */
  KDTree_3d *tree = NULL;
  MirrTopoStore_t mesh_topo_store = {NULL, -1, -1, -1};

  BM_mesh_elem_table_ensure(bm, BM_VERT);

  if (r_index == NULL) {
    const char *layer_id = BM_CD_LAYER_ID;
    em->mirror_cdlayer = CustomData_get_named_layer_index(&bm->vdata, CD_PROP_INT32, layer_id);
    if (em->mirror_cdlayer == -1) {
      BM_data_layer_add_named(bm, &bm->vdata, CD_PROP_INT32, layer_id);
      em->mirror_cdlayer = CustomData_get_named_layer_index(&bm->vdata, CD_PROP_INT32, layer_id);
    }

    cd_vmirr_offset = CustomData_get_n_offset(
        &bm->vdata,
        CD_PROP_INT32,
        em->mirror_cdlayer - CustomData_get_layer_index(&bm->vdata, CD_PROP_INT32));

    bm->vdata.layers[em->mirror_cdlayer].flag |= CD_FLAG_TEMPORARY;
  }

  BM_mesh_elem_index_ensure(bm, BM_VERT);

  if (use_topology) {
    ED_mesh_mirrtopo_init(em, NULL, &mesh_topo_store, true);
  }
  else {
    tree = BLI_kdtree_3d_new(bm->totvert);
    BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
      if (respecthide && BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
        continue;
      }

      BLI_kdtree_3d_insert(tree, i, v->co);
    }
    BLI_kdtree_3d_balance(tree);
  }

#define VERT_INTPTR(_v, _i) (r_index ? &r_index[_i] : BM_ELEM_CD_GET_VOID_P(_v, cd_vmirr_offset))

  BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
    if (respecthide && BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      continue;
    }

    if (use_select && !BM_elem_flag_test(v, BM_ELEM_SELECT)) {
      continue;
    }

    BLI_assert(BM_elem_index_get(v) == i);
    BMVert *v_mirr;
    int *idx = VERT_INTPTR(v, i);

    if (use_topology) {
      v_mirr = cache_mirr_intptr_as_bmvert(mesh_topo_store.index_lookup, i);
      if (v_mirr != NULL) {
        if (respecthide && BM_elem_flag_test(v_mirr, BM_ELEM_HIDDEN)) {
          v_mirr = NULL;
        }
      }
    }
    else {
      int i_mirr;
      float co[3];
      copy_v3_v3(co, v->co);
      co[axis] *= -1.0f;

      v_mirr = NULL;
      i_mirr = BLI_kdtree_3d_find_nearest(tree, co, NULL);
      if (i_mirr != -1) {
        BMVert *v_test = BM_vert_at_index(bm, i_mirr);
        if (len_squared_v3v3(co, v_test->co) < maxdist_sq) {
          v_mirr = v_test;
        }
      }
    }

    if (v_mirr && (use_self || (v_mirr != v))) {
      const int i_mirr = BM_elem_index_get(v_mirr);
      *idx = i_mirr;
      idx = VERT_INTPTR(v_mirr, i_mirr);
      *idx = i;
    }
    else {
      *idx = -1;
    }
  }

#undef VERT_INTPTR

  if (use_topology) {
    ED_mesh_mirrtopo_free(&mesh_topo_store);
  }
  else {
    BLI_kdtree_3d_free(tree);
  }
}

void EDBM_verts_mirror_cache_begin(BMEditMesh *em,
                                   const int axis,
                                   const bool use_self,
                                   const bool use_select,
                                   const bool respecthide,
                                   const bool use_topology)
{
  EDBM_verts_mirror_cache_begin_ex(em,
                                   axis,
                                   use_self,
                                   use_select,
                                   respecthide,
                                   /* extra args */
                                   use_topology,
                                   BM_SEARCH_MAXDIST_MIRR,
                                   NULL);
}

BMVert *EDBM_verts_mirror_get(BMEditMesh *em, BMVert *v)
{
  const int *mirr = CustomData_bmesh_get_layer_n(&em->bm->vdata, v->head.data, em->mirror_cdlayer);

  BLI_assert(em->mirror_cdlayer != -1); /* invalid use */

  if (mirr && *mirr >= 0 && *mirr < em->bm->totvert) {
    if (!em->bm->vtable) {
      printf(
          "err: should only be called between "
          "EDBM_verts_mirror_cache_begin and EDBM_verts_mirror_cache_end");
      return NULL;
    }

    return em->bm->vtable[*mirr];
  }

  return NULL;
}

BMEdge *EDBM_verts_mirror_get_edge(BMEditMesh *em, BMEdge *e)
{
  BMVert *v1_mirr = EDBM_verts_mirror_get(em, e->v1);
  if (v1_mirr) {
    BMVert *v2_mirr = EDBM_verts_mirror_get(em, e->v2);
    if (v2_mirr) {
      return BM_edge_exists(v1_mirr, v2_mirr);
    }
  }

  return NULL;
}

BMFace *EDBM_verts_mirror_get_face(BMEditMesh *em, BMFace *f)
{
  BMVert **v_mirr_arr = BLI_array_alloca(v_mirr_arr, f->len);

  BMLoop *l_iter, *l_first;
  uint i = 0;

  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    if ((v_mirr_arr[i++] = EDBM_verts_mirror_get(em, l_iter->v)) == NULL) {
      return NULL;
    }
  } while ((l_iter = l_iter->next) != l_first);

  return BM_face_exists(v_mirr_arr, f->len);
}

void EDBM_verts_mirror_cache_clear(BMEditMesh *em, BMVert *v)
{
  int *mirr = CustomData_bmesh_get_layer_n(&em->bm->vdata, v->head.data, em->mirror_cdlayer);

  BLI_assert(em->mirror_cdlayer != -1); /* invalid use */

  if (mirr) {
    *mirr = -1;
  }
}

void EDBM_verts_mirror_cache_end(BMEditMesh *em)
{
  em->mirror_cdlayer = -1;
}

void EDBM_verts_mirror_apply(BMEditMesh *em, const int sel_from, const int sel_to)
{
  BMIter iter;
  BMVert *v;

  BLI_assert((em->bm->vtable != NULL) && ((em->bm->elem_table_dirty & BM_VERT) == 0));

  BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, BM_ELEM_SELECT) == sel_from) {
      BMVert *mirr = EDBM_verts_mirror_get(em, v);
      if (mirr) {
        if (BM_elem_flag_test(mirr, BM_ELEM_SELECT) == sel_to) {
          copy_v3_v3(mirr->co, v->co);
          mirr->co[0] *= -1.0f;
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hide/Reveal API
 * \{ */

/* swap is 0 or 1, if 1 it hides not selected */
bool EDBM_mesh_hide(BMEditMesh *em, bool swap)
{
  BMIter iter;
  BMElem *ele;
  int itermode;
  char hflag_swap = swap ? BM_ELEM_SELECT : 0;
  bool changed = true;

  if (em->selectmode & SCE_SELECT_VERTEX) {
    itermode = BM_VERTS_OF_MESH;
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    itermode = BM_EDGES_OF_MESH;
  }
  else {
    itermode = BM_FACES_OF_MESH;
  }

  BM_ITER_MESH (ele, &iter, em->bm, itermode) {
    if (!BM_elem_flag_test(ele, BM_ELEM_HIDDEN)) {
      if (BM_elem_flag_test(ele, BM_ELEM_SELECT) ^ hflag_swap) {
        BM_elem_hide_set(em->bm, ele, true);
        changed = true;
      }
    }
  }

  if (changed) {
    EDBM_selectmode_flush(em);
  }
  return changed;

  /* original hide flushing comment (OUTDATED):
   * hide happens on least dominant select mode, and flushes up, not down!
   * (helps preventing errors in subsurf) */
  /* - vertex hidden, always means edge is hidden too
   * - edge hidden, always means face is hidden too
   * - face hidden, only set face hide
   * - then only flush back down what's absolute hidden
   */
}

bool EDBM_mesh_reveal(BMEditMesh *em, bool select)
{
  const char iter_types[3] = {
      BM_VERTS_OF_MESH,
      BM_EDGES_OF_MESH,
      BM_FACES_OF_MESH,
  };

  const bool sels[3] = {
      (em->selectmode & SCE_SELECT_VERTEX) != 0,
      (em->selectmode & SCE_SELECT_EDGE) != 0,
      (em->selectmode & SCE_SELECT_FACE) != 0,
  };
  int i;
  bool changed = false;

  /* Use tag flag to remember what was hidden before all is revealed.
   * BM_ELEM_HIDDEN --> BM_ELEM_TAG */
  for (i = 0; i < 3; i++) {
    BMIter iter;
    BMElem *ele;

    BM_ITER_MESH (ele, &iter, em->bm, iter_types[i]) {
      if (BM_elem_flag_test(ele, BM_ELEM_HIDDEN)) {
        BM_elem_flag_enable(ele, BM_ELEM_TAG);
        changed = true;
      }
      else {
        BM_elem_flag_disable(ele, BM_ELEM_TAG);
      }
    }
  }

  if (!changed) {
    return false;
  }

  /* Reveal everything */
  EDBM_flag_disable_all(em, BM_ELEM_HIDDEN);

  /* Select relevant just-revealed elements */
  for (i = 0; i < 3; i++) {
    BMIter iter;
    BMElem *ele;

    if (!sels[i]) {
      continue;
    }

    BM_ITER_MESH (ele, &iter, em->bm, iter_types[i]) {
      if (BM_elem_flag_test(ele, BM_ELEM_TAG)) {
        BM_elem_select_set(em->bm, ele, select);
      }
    }
  }

  EDBM_selectmode_flush(em);

  /* hidden faces can have invalid normals */
  EDBM_mesh_normals_update(em);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Update API
 * \{ */

void EDBM_mesh_normals_update_ex(BMEditMesh *em, const struct BMeshNormalsUpdate_Params *params)
{
  BM_mesh_normals_update_ex(em->bm, params);
}

void EDBM_mesh_normals_update(BMEditMesh *em)
{
  EDBM_mesh_normals_update_ex(em,
                              &(const struct BMeshNormalsUpdate_Params){
                                  .face_normals = true,
                              });
}

void EDBM_stats_update(BMEditMesh *em)
{
  const char iter_types[3] = {
      BM_VERTS_OF_MESH,
      BM_EDGES_OF_MESH,
      BM_FACES_OF_MESH,
  };

  BMIter iter;
  BMElem *ele;
  int *tots[3];
  int i;

  tots[0] = &em->bm->totvertsel;
  tots[1] = &em->bm->totedgesel;
  tots[2] = &em->bm->totfacesel;

  em->bm->totvertsel = em->bm->totedgesel = em->bm->totfacesel = 0;

  for (i = 0; i < 3; i++) {
    ele = BM_iter_new(&iter, em->bm, iter_types[i], NULL);
    for (; ele; ele = BM_iter_step(&iter)) {
      if (BM_elem_flag_test(ele, BM_ELEM_SELECT)) {
        (*tots[i])++;
      }
    }
  }
}

/**
 * So many tools call these that we better make it a generic function.
 */
void EDBM_update(Mesh *mesh, const struct EDBMUpdate_Params *params)
{
  BMEditMesh *em = mesh->edit_mesh;
  /* Order of calling isn't important. */
  DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh->id);

  if (params->calc_normals && params->calc_looptri) {
    /* Calculating both has some performance gains. */
    BKE_editmesh_looptri_and_normals_calc(em);
  }
  else {
    if (params->calc_normals) {
      EDBM_mesh_normals_update(em);
    }

    if (params->calc_looptri) {
      BKE_editmesh_looptri_calc(em);
    }
  }

  if (params->is_destructive) {
    /* TODO. we may be able to remove this now! - Campbell */
    // BM_mesh_elem_table_free(em->bm, BM_ALL_NOLOOP);
  }
  else {
    /* in debug mode double check we didn't need to recalculate */
    BLI_assert(BM_mesh_elem_table_check(em->bm) == true);
  }
  if (em->bm->spacearr_dirty & BM_SPACEARR_BMO_SET) {
    BM_lnorspace_invalidate(em->bm, false);
    em->bm->spacearr_dirty &= ~BM_SPACEARR_BMO_SET;
  }
  /* don't keep stale derivedMesh data around, see: T38872. */
  BKE_editmesh_free_derivedmesh(em);

#ifdef DEBUG
  {
    BMEditSelection *ese;
    for (ese = em->bm->selected.first; ese; ese = ese->next) {
      BLI_assert(BM_elem_flag_test(ese->ele, BM_ELEM_SELECT));
    }
  }
#endif
}

/* Bad level call from Python API. */
void EDBM_update_extern(struct Mesh *me, const bool do_tessellation, const bool is_destructive)
{
  EDBM_update(me,
              &(const struct EDBMUpdate_Params){
                  .calc_looptri = do_tessellation,
                  .calc_normals = false,
                  .is_destructive = is_destructive,
              });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Helpers
 * \{ */

/* poll call for mesh operators requiring a view3d context */
bool EDBM_view3d_poll(bContext *C)
{
  if (ED_operator_editmesh(C) && ED_operator_view3d_active(C)) {
    return 1;
  }

  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BMesh Element API
 * \{ */

BMElem *EDBM_elem_from_selectmode(BMEditMesh *em, BMVert *eve, BMEdge *eed, BMFace *efa)
{
  BMElem *ele = NULL;

  if ((em->selectmode & SCE_SELECT_VERTEX) && eve) {
    ele = (BMElem *)eve;
  }
  else if ((em->selectmode & SCE_SELECT_EDGE) && eed) {
    ele = (BMElem *)eed;
  }
  else if ((em->selectmode & SCE_SELECT_FACE) && efa) {
    ele = (BMElem *)efa;
  }

  return ele;
}

/**
 * Used when we want to store a single index for any vert/edge/face.
 *
 * Intended for use with operators.
 */
int EDBM_elem_to_index_any(BMEditMesh *em, BMElem *ele)
{
  BMesh *bm = em->bm;
  int index = BM_elem_index_get(ele);

  if (ele->head.htype == BM_VERT) {
    BLI_assert(!(bm->elem_index_dirty & BM_VERT));
  }
  else if (ele->head.htype == BM_EDGE) {
    BLI_assert(!(bm->elem_index_dirty & BM_EDGE));
    index += bm->totvert;
  }
  else if (ele->head.htype == BM_FACE) {
    BLI_assert(!(bm->elem_index_dirty & BM_FACE));
    index += bm->totvert + bm->totedge;
  }
  else {
    BLI_assert(0);
  }

  return index;
}

BMElem *EDBM_elem_from_index_any(BMEditMesh *em, int index)
{
  BMesh *bm = em->bm;

  if (index < bm->totvert) {
    return (BMElem *)BM_vert_at_index_find_or_table(bm, index);
  }
  index -= bm->totvert;
  if (index < bm->totedge) {
    return (BMElem *)BM_edge_at_index_find_or_table(bm, index);
  }
  index -= bm->totedge;
  if (index < bm->totface) {
    return (BMElem *)BM_face_at_index_find_or_table(bm, index);
  }

  return NULL;
}

int EDBM_elem_to_index_any_multi(ViewLayer *view_layer,
                                 BMEditMesh *em,
                                 BMElem *ele,
                                 int *r_object_index)
{
  uint bases_len;
  int elem_index = -1;
  *r_object_index = -1;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode(view_layer, NULL, &bases_len);
  for (uint base_index = 0; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    if (BKE_editmesh_from_object(base_iter->object) == em) {
      *r_object_index = base_index;
      elem_index = EDBM_elem_to_index_any(em, ele);
      break;
    }
  }
  MEM_freeN(bases);
  return elem_index;
}

BMElem *EDBM_elem_from_index_any_multi(ViewLayer *view_layer,
                                       int object_index,
                                       int elem_index,
                                       Object **r_obedit)
{
  uint bases_len;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode(view_layer, NULL, &bases_len);
  *r_obedit = NULL;
  Object *obedit = ((uint)object_index < bases_len) ? bases[object_index]->object : NULL;
  MEM_freeN(bases);
  if (obedit != NULL) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMElem *ele = EDBM_elem_from_index_any(em, elem_index);
    if (ele != NULL) {
      *r_obedit = obedit;
      return ele;
    }
  }
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BMesh BVH API
 * \{ */

static BMFace *edge_ray_cast(
    struct BMBVHTree *tree, const float co[3], const float dir[3], float *r_hitout, BMEdge *e)
{
  BMFace *f = BKE_bmbvh_ray_cast(tree, co, dir, 0.0f, NULL, r_hitout, NULL);

  if (f && BM_edge_in_face(e, f)) {
    return NULL;
  }

  return f;
}

static void scale_point(float c1[3], const float p[3], const float s)
{
  sub_v3_v3(c1, p);
  mul_v3_fl(c1, s);
  add_v3_v3(c1, p);
}

bool BMBVH_EdgeVisible(struct BMBVHTree *tree,
                       BMEdge *e,
                       struct Depsgraph *depsgraph,
                       ARegion *region,
                       View3D *v3d,
                       Object *obedit)
{
  BMFace *f;
  float co1[3], co2[3], co3[3], dir1[3], dir2[3], dir3[3];
  float origin[3], invmat[4][4];
  float epsilon = 0.01f;
  float end[3];
  const float mval_f[2] = {
      region->winx / 2.0f,
      region->winy / 2.0f,
  };

  ED_view3d_win_to_segment_clipped(depsgraph, region, v3d, mval_f, origin, end, false);

  invert_m4_m4(invmat, obedit->obmat);
  mul_m4_v3(invmat, origin);

  copy_v3_v3(co1, e->v1->co);
  mid_v3_v3v3(co2, e->v1->co, e->v2->co);
  copy_v3_v3(co3, e->v2->co);

  scale_point(co1, co2, 0.99);
  scale_point(co3, co2, 0.99);

  /* ok, idea is to generate rays going from the camera origin to the
   * three points on the edge (v1, mid, v2)*/
  sub_v3_v3v3(dir1, origin, co1);
  sub_v3_v3v3(dir2, origin, co2);
  sub_v3_v3v3(dir3, origin, co3);

  normalize_v3_length(dir1, epsilon);
  normalize_v3_length(dir2, epsilon);
  normalize_v3_length(dir3, epsilon);

  /* offset coordinates slightly along view vectors, to avoid
   * hitting the faces that own the edge.*/
  add_v3_v3v3(co1, co1, dir1);
  add_v3_v3v3(co2, co2, dir2);
  add_v3_v3v3(co3, co3, dir3);

  normalize_v3(dir1);
  normalize_v3(dir2);
  normalize_v3(dir3);

  /* do three samplings: left, middle, right */
  f = edge_ray_cast(tree, co1, dir1, NULL, e);
  if (f && !edge_ray_cast(tree, co2, dir2, NULL, e)) {
    return true;
  }
  if (f && !edge_ray_cast(tree, co3, dir3, NULL, e)) {
    return true;
  }
  if (!f) {
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name BMesh Vertex Projection API
 * \{ */

void EDBM_project_snap_verts(
    bContext *C, Depsgraph *depsgraph, ARegion *region, Object *obedit, BMEditMesh *em)
{
  BMIter iter;
  BMVert *eve;

  ED_view3d_init_mats_rv3d(obedit, region->regiondata);

  struct SnapObjectContext *snap_context = ED_transform_snap_object_context_create_view3d(
      CTX_data_scene(C), 0, region, CTX_wm_view3d(C));

  BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
      float mval[2], co_proj[3];
      if (ED_view3d_project_float_object(region, eve->co, mval, V3D_PROJ_TEST_NOP) ==
          V3D_PROJ_RET_OK) {
        if (ED_transform_snap_object_project_view3d(snap_context,
                                                    depsgraph,
                                                    SCE_SNAP_MODE_FACE,
                                                    &(const struct SnapObjectParams){
                                                        .snap_select = SNAP_NOT_ACTIVE,
                                                        .edit_mode_type = SNAP_GEOM_FINAL,
                                                        .use_occlusion_test = true,
                                                    },
                                                    mval,
                                                    NULL,
                                                    NULL,
                                                    co_proj,
                                                    NULL)) {
          mul_v3_m4v3(eve->co, obedit->imat, co_proj);
        }
      }
    }
  }

  ED_transform_snap_object_context_destroy(snap_context);
}

/** \} */
