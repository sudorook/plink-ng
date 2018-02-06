// This file is part of PLINK 2.00, copyright (C) 2005-2018 Shaun Purcell,
// Christopher Chang.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#include "plink2_compress_stream.h"
#include "plink2_matrix.h"
#include "plink2_matrix_calc.h"
#include "plink2_random.h"

#ifdef __cplusplus
namespace plink2 {
#endif

void init_score(score_info_t* score_info_ptr) {
  score_info_ptr->flags = kfScore0;
  score_info_ptr->varid_col_p1 = 1;
  score_info_ptr->allele_col_p1 = 0;
  score_info_ptr->input_fname = nullptr;
  init_range_list(&(score_info_ptr->input_col_idx_range_list));
}

void cleanup_score(score_info_t* score_info_ptr) {
  free_cond(score_info_ptr->input_fname);
  cleanup_range_list(&(score_info_ptr->input_col_idx_range_list));
}


uint32_t triangle_divide(int64_t cur_prod_x2, int32_t modif) {
  // return smallest integer vv for which (vv * (vv + modif)) is no smaller
  // than cur_prod_x2, and neither term in the product is negative.
  int64_t vv;
  if (cur_prod_x2 == 0) {
    if (modif < 0) {
      return -modif;
    }
    return 0;
  }
  vv = S_CAST(int64_t, sqrt(S_CAST(double, cur_prod_x2)));
  while ((vv - 1) * (vv + modif - 1) >= cur_prod_x2) {
    vv--;
  }
  while (vv * (vv + modif) < cur_prod_x2) {
    vv++;
  }
  return vv;
}

void parallel_bounds(uint32_t ct, int32_t start, uint32_t parallel_idx, uint32_t parallel_tot, int32_t* __restrict bound_start_ptr, int32_t* __restrict bound_end_ptr) {
  int32_t modif = 1 - start * 2;
  int64_t ct_tot = S_CAST(int64_t, ct) * (ct + modif);
  *bound_start_ptr = triangle_divide((ct_tot * parallel_idx) / parallel_tot, modif);
  *bound_end_ptr = triangle_divide((ct_tot * (parallel_idx + 1)) / parallel_tot, modif);
}

// set align to 1 for no alignment
void triangle_fill(uint32_t ct, uint32_t piece_ct, uint32_t parallel_idx, uint32_t parallel_tot, uint32_t start, uint32_t align, uint32_t* target_arr) {
  int32_t modif = 1 - start * 2;
  int64_t cur_prod_x2;
  int32_t lbound;
  int32_t ubound;
  uint32_t uii;
  uint32_t align_m1;
  parallel_bounds(ct, start, parallel_idx, parallel_tot, &lbound, &ubound);
  // x(x+1)/2 is divisible by y iff (x % (2y)) is 0 or (2y - 1).
  align *= 2;
  align_m1 = align - 1;
  target_arr[0] = lbound;
  target_arr[piece_ct] = ubound;
  cur_prod_x2 = S_CAST(int64_t, lbound) * (lbound + modif);
  const int64_t ct_tr = (S_CAST(int64_t, ubound) * (ubound + modif) - cur_prod_x2) / piece_ct;
  for (uint32_t piece_idx = 1; piece_idx < piece_ct; ++piece_idx) {
    cur_prod_x2 += ct_tr;
    lbound = triangle_divide(cur_prod_x2, modif);
    uii = (lbound - S_CAST(int32_t, start)) & align_m1;
    if ((uii) && (uii != align_m1)) {
      lbound = start + ((lbound - S_CAST(int32_t, start)) | align_m1);
    }
    // lack of this check caused a nasty bug earlier
    if (S_CAST(uint32_t, lbound) > ct) {
      lbound = ct;
    }
    target_arr[piece_idx] = lbound;
  }
}

// Returns 0 if cells_avail is insufficient.
uint32_t count_triangle_passes(uintptr_t start_idx, uintptr_t end_idx, uintptr_t is_no_diag, uintptr_t cells_avail) {
  start_idx -= is_no_diag;
  end_idx -= is_no_diag;
  if (cells_avail < end_idx) {
    return 0;
  }
  cells_avail *= 2; // don't want to worry about /2 in triangular numbers
  const uint64_t end_tri = S_CAST(uint64_t, end_idx) * (end_idx + 1);
  uint64_t start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
  uint32_t pass_ct = 0;
  while (1) {
    ++pass_ct;
    const uint64_t delta_tri = end_tri - start_tri;
    if (delta_tri <= cells_avail) {
      return pass_ct;
    }
    const uint64_t next_target = start_tri + cells_avail;
    start_idx = S_CAST(int64_t, sqrt(u63tod(next_target)));
    start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
    if (start_tri > next_target) {
      --start_idx;
      start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
      assert(start_tri <= next_target);
    }
  }
}

uint64_t next_triangle_pass(uintptr_t start_idx, uintptr_t grand_end_idx, uintptr_t is_no_diag, uintptr_t cells_avail) {
  cells_avail *= 2;
  start_idx -= is_no_diag;
  grand_end_idx -= is_no_diag;
  const uint64_t end_tri = S_CAST(uint64_t, grand_end_idx) * (grand_end_idx + 1);
  uint64_t start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
  const uint64_t delta_tri = end_tri - start_tri;
  if (delta_tri <= cells_avail) {
    return grand_end_idx + is_no_diag;
  }
  const uint64_t next_target = start_tri + cells_avail;
  start_idx = S_CAST(int64_t, sqrt(u63tod(next_target)));
  start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
  return start_idx + is_no_diag - (start_tri > next_target);
}

void triangle_load_balance(uint32_t piece_ct, uintptr_t start_idx, uintptr_t end_idx, uint32_t is_no_diag, uint32_t* target_arr) {
  target_arr[0] = start_idx;
  target_arr[piece_ct] = end_idx;
  start_idx -= is_no_diag;
  end_idx -= is_no_diag;
  const uint64_t end_tri = S_CAST(uint64_t, end_idx) * (end_idx + 1);
  uint64_t cur_target = S_CAST(uint64_t, start_idx) * (start_idx + 1);
  const uint64_t std_size = (end_tri - cur_target) / piece_ct;
  for (uint32_t piece_idx = 1; piece_idx < piece_ct; ++piece_idx) {
    // don't use cur_target = start_tri + (piece_idx * delta_tri) / piece_ct
    // because of potential overflow
    cur_target += std_size;
    start_idx = S_CAST(int64_t, sqrt(u63tod(cur_target)));
    const uint64_t start_tri = S_CAST(uint64_t, start_idx) * (start_idx + 1);
    if (start_tri > cur_target) {
      --start_idx;
    }
    target_arr[piece_idx] = start_idx + is_no_diag;
  }
}

pglerr_t kinship_prune_destructive(uintptr_t* kinship_table, uintptr_t* sample_include, uint32_t* sample_ct_ptr) {
  pglerr_t reterr = kPglRetSuccess;
  {
    const uintptr_t orig_sample_ct = *sample_ct_ptr;
    const uintptr_t orig_sample_ctl = BITCT_TO_WORDCT(orig_sample_ct);
    uintptr_t* sample_include_collapsed_nz;
    uintptr_t* sample_remove_collapsed;
    uint32_t* vertex_degree;
    if (bigstack_calloc_ul(orig_sample_ctl, &sample_include_collapsed_nz) ||
        bigstack_calloc_ul(orig_sample_ctl, &sample_remove_collapsed) ||
        bigstack_alloc_ui(orig_sample_ct, &vertex_degree)) {
      goto kinship_prune_destructive_ret_NOMEM;
    }
    // 1. count the number of constraints for each remaining sample
    uint32_t degree_1_vertex_ct = 0;
    for (uint32_t sample_idx = 0; sample_idx < orig_sample_ct; ++sample_idx) {
      const uintptr_t woffset = sample_idx * orig_sample_ctl;
      const uintptr_t* read_iter1 = &(kinship_table[woffset]);
      // don't currently guarantee vector-alignment of kinship_table rows, so
      // can't use popcount_longs().  (change this?)
      uint32_t cur_degree = 0;
      for (uint32_t widx = 0; widx < orig_sample_ctl; ++widx) {
        const uintptr_t cur_word = *read_iter1++;
        cur_degree += popcount_long(cur_word);
      }
      if (cur_degree) {
        vertex_degree[sample_idx] = cur_degree;
        degree_1_vertex_ct += (cur_degree == 1);
        SET_BIT(sample_idx, sample_include_collapsed_nz);
      }
    }
    uint32_t cur_sample_nz_ct = popcount_longs(sample_include_collapsed_nz, orig_sample_ctl);
    // 2. as long as edges remain,
    //    a. remove partner of first degree-one vertex, if such a vertex exists
    //    b. otherwise, remove first maximal-degree vertex
    //    (similar to plink 1.9 rel_cutoff_batch(), but data structure is not
    //    triangular since more speed is needed)
    while (cur_sample_nz_ct) {
      uint32_t prune_uidx;
      uint32_t cur_degree;
      if (degree_1_vertex_ct) {
        uint32_t degree_1_vertex_uidx = 0;
        while (1) {
          // sparse
          degree_1_vertex_uidx = next_set_unsafe(sample_include_collapsed_nz, degree_1_vertex_uidx);
          if (vertex_degree[degree_1_vertex_uidx] == 1) {
            break;
          }
          ++degree_1_vertex_uidx;
        }
        // find partner
        prune_uidx = next_set_unsafe(&(kinship_table[degree_1_vertex_uidx * orig_sample_ctl]), 0);
        cur_degree = vertex_degree[prune_uidx];
      } else {
        uint32_t sample_uidx = next_set_unsafe(sample_include_collapsed_nz, 0);
        cur_degree = vertex_degree[sample_uidx];
        prune_uidx = sample_uidx;
        for (uint32_t sample_idx = 1; sample_idx < cur_sample_nz_ct; ++sample_idx) {
          // sparse
          sample_uidx = next_set_unsafe(sample_include_collapsed_nz, sample_uidx + 1);
          const uint32_t new_degree = vertex_degree[sample_uidx];
          if (new_degree > cur_degree) {
            cur_degree = new_degree;
            prune_uidx = sample_uidx;
          }
        }
      }
      // remove row/column
      uintptr_t* cur_kinship_col = &(kinship_table[prune_uidx / kBitsPerWord]);
      const uintptr_t kinship_col_mask = ~(k1LU << (prune_uidx % kBitsPerWord));
      uintptr_t* cur_kinship_row = &(kinship_table[prune_uidx * orig_sample_ctl]);
      uint32_t sample_uidx = 0;
      for (uint32_t partner_idx = 0; partner_idx < cur_degree; ++partner_idx, ++sample_uidx) {
        // sparse
        sample_uidx = next_set_unsafe(cur_kinship_row, sample_uidx);
        const uint32_t new_degree = vertex_degree[sample_uidx] - 1;
        if (!new_degree) {
          CLEAR_BIT(sample_uidx, sample_include_collapsed_nz);
          --degree_1_vertex_ct;
          --cur_sample_nz_ct;
          // unnecessary to write to kinship_table[] or vertex_degree[]
        } else {
          cur_kinship_col[sample_uidx * orig_sample_ctl] &= kinship_col_mask;
          degree_1_vertex_ct += (new_degree == 1);
          vertex_degree[sample_uidx] = new_degree;
        }
      }
      if (vertex_degree[prune_uidx] == 1) {
        --degree_1_vertex_ct;
      }
      sample_remove_collapsed[prune_uidx / kBitsPerWord] |= ~kinship_col_mask;
      sample_include_collapsed_nz[prune_uidx / kBitsPerWord] &= kinship_col_mask;
      // unnecessary to update current kinship_table[] row
      --cur_sample_nz_ct;
    }
    uint32_t sample_ct = orig_sample_ct;
    uint32_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < orig_sample_ct; ++sample_idx, ++sample_uidx) {
      next_set_unsafe_ck(sample_include, &sample_uidx);
      if (IS_SET(sample_remove_collapsed, sample_idx)) {
        CLEAR_BIT(sample_uidx, sample_include);
        --sample_ct;
      }
    }
    *sample_ct_ptr = sample_ct;
  }
  while (0) {
  kinship_prune_destructive_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  }
  return reterr;
}

pglerr_t king_cutoff_batch(const char* sample_ids, const char* sids, uint32_t raw_sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, double king_cutoff, uintptr_t* sample_include, char* king_cutoff_fprefix, uint32_t* sample_ct_ptr) {
  unsigned char* bigstack_mark = g_bigstack_base;
  gzFile gz_infile = nullptr;
  FILE* binfile = nullptr;
  uintptr_t line_idx = 0;
  pglerr_t reterr = kPglRetSuccess;
  {
    uint32_t sample_ct = *sample_ct_ptr;
    const uint32_t orig_sample_ctl = BITCT_TO_WORDCT(sample_ct);
    uintptr_t* kinship_table;
    if (bigstack_calloc_ul(sample_ct * orig_sample_ctl, &kinship_table)) {
      goto king_cutoff_batch_ret_NOMEM;
    }

    char* fprefix_end = &(king_cutoff_fprefix[strlen(king_cutoff_fprefix)]);
    snprintf(fprefix_end, 9, ".king.id");
    reterr = gzopen_read_checked(king_cutoff_fprefix, &gz_infile);
    if (reterr) {
      goto king_cutoff_batch_ret_1;
    }

    uint32_t* sample_uidx_to_king_uidx;
    char* loadbuf;
    if (bigstack_alloc_ui(raw_sample_ct, &sample_uidx_to_king_uidx) ||
        bigstack_alloc_c(kMaxMediumLine, &loadbuf)) {
      goto king_cutoff_batch_ret_NOMEM;
    }
    ++line_idx;
    loadbuf[kMaxMediumLine - 1] = ' ';
    if (!gzgets(gz_infile, loadbuf, kMaxMediumLine)) {
      if (!gzeof(gz_infile)) {
        goto king_cutoff_batch_ret_READ_FAIL;
      }
      logerrprint("Error: Empty --king-cutoff ID file.\n");
      goto king_cutoff_batch_ret_MALFORMED_INPUT;
    }
    if (!loadbuf[kMaxMediumLine - 1]) {
      goto king_cutoff_batch_ret_LONG_LINE;
    }
    const char* loadbuf_first_token = skip_initial_spaces(loadbuf);
    if (is_eoln_kns(*loadbuf_first_token)) {
      goto king_cutoff_batch_ret_MISSING_TOKENS;
    }
    const xid_mode_t xid_mode = (sids && next_token_mult(loadbuf_first_token, 2))? kfXidModeFidiidSid : kfXidModeFidiid;

    uint32_t* xid_map; // IDs not collapsed
    char* sorted_xidbox;
    uintptr_t max_xid_blen;
    reterr = sorted_xidbox_init_alloc(sample_include, sample_ids, sids, sample_ct, max_sample_id_blen, max_sid_blen, 0, xid_mode, 0, &sorted_xidbox, &xid_map, &max_xid_blen);
    if (reterr) {
      goto king_cutoff_batch_ret_1;
    }
    char* idbuf;
    if (bigstack_alloc_c(max_xid_blen, &idbuf)) {
      goto king_cutoff_batch_ret_NOMEM;
    }
    fill_uint_one(raw_sample_ct, sample_uidx_to_king_uidx);
    while (1) {
      const char* loadbuf_iter = loadbuf_first_token;
      uint32_t sample_uidx;
      if (!sorted_xidbox_read_find(sorted_xidbox, xid_map, max_xid_blen, sample_ct, 0, xid_mode, &loadbuf_iter, &sample_uidx, idbuf)) {
        if (sample_uidx_to_king_uidx[sample_uidx] != UINT32_MAX) {
          char* first_tab = S_CAST(char*, rawmemchr(idbuf, '\t'));
          char* second_tab = strchr(&(first_tab[1]), '\t');
          *first_tab = ' ';
          if (second_tab) {
            *second_tab = ' ';
          }
          snprintf(g_logbuf, kLogbufSize, "Error: Duplicate ID '%s' in %s .\n", idbuf, king_cutoff_fprefix);
          goto king_cutoff_batch_ret_MALFORMED_INPUT_WW;
        }
        sample_uidx_to_king_uidx[sample_uidx] = line_idx - 1;
      } else {
        if (!loadbuf_iter) {
          goto king_cutoff_batch_ret_MISSING_TOKENS;
        }
      }

      ++line_idx;
      if (!gzgets(gz_infile, loadbuf, kMaxMediumLine)) {
        if (!gzeof(gz_infile)) {
          goto king_cutoff_batch_ret_READ_FAIL;
        }
        break;
      }
      if (!loadbuf[kMaxMediumLine - 1]) {
        goto king_cutoff_batch_ret_LONG_LINE;
      }
      loadbuf_first_token = skip_initial_spaces(loadbuf);
      if (is_eoln_kns(*loadbuf_first_token)) {
        goto king_cutoff_batch_ret_MISSING_TOKENS;
      }
    }
    if (gzclose_null(&gz_infile)) {
      goto king_cutoff_batch_ret_READ_FAIL;
    }
    const uintptr_t king_id_ct = line_idx - 1;

    bigstack_reset(loadbuf);
    uintptr_t* king_include;
    uint32_t* king_uidx_to_sample_idx;
    if (bigstack_calloc_ul(BITCT_TO_WORDCT(king_id_ct), &king_include) ||
        bigstack_alloc_ui(king_id_ct, &king_uidx_to_sample_idx)) {
      goto king_cutoff_batch_ret_NOMEM;
    }
    uint32_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
      next_set_unsafe_ck(sample_include, &sample_uidx);
      const uint32_t king_uidx = sample_uidx_to_king_uidx[sample_uidx];
      if (king_uidx != UINT32_MAX) {
        SET_BIT(king_uidx, king_include);
        king_uidx_to_sample_idx[king_uidx] = sample_idx;
      }
    }
    snprintf(fprefix_end, 9, ".king.bin");
    if (fopen_checked(king_cutoff_fprefix, FOPEN_RB, &binfile)) {
      goto king_cutoff_batch_ret_OPEN_FAIL;
    }
    if (fseeko(binfile, 0, SEEK_END)) {
      goto king_cutoff_batch_ret_READ_FAIL;
    }
    const uint64_t fsize = ftello(binfile);
    const uint64_t fsize_double_expected = (king_id_ct * (S_CAST(uint64_t, king_id_ct) - 1) * (sizeof(double) / 2));
    const uint32_t is_double = (fsize == fsize_double_expected);
    rewind(binfile);
    const uint32_t first_king_uidx = next_set(king_include, 0, king_id_ct);
    uintptr_t king_uidx = next_set(king_include, first_king_uidx + 1, king_id_ct);
    if (king_uidx > 1) {
      if (fseeko(binfile, king_uidx * (S_CAST(uint64_t, king_uidx) - 1) * (2 + (2 * is_double)), SEEK_SET)) {
        goto king_cutoff_batch_ret_READ_FAIL;
      }
    }
    uintptr_t constraint_ct = 0;
    if (is_double) {
      // fread limit
      assert(king_id_ct <= ((kMaxBytesPerIO / sizeof(double)) + 1));
      double* king_drow;
      if (bigstack_alloc_d(king_id_ct - 1, &king_drow)) {
        goto king_cutoff_batch_ret_NOMEM;
      }
      for (uint32_t king_idx = 1; king_uidx < king_id_ct; ++king_idx, ++king_uidx) {
        if (!IS_SET(king_include, king_uidx)) {
          king_uidx = next_set(king_include, king_uidx + 1, king_id_ct);
          if (king_uidx == king_id_ct) {
            break;
          }
          if (fseeko(binfile, S_CAST(uint64_t, king_uidx) * (king_uidx - 1) * (sizeof(double) / 2), SEEK_SET)) {
            goto king_cutoff_batch_ret_READ_FAIL;
          }
        }
        if (!fread_unlocked(king_drow, king_uidx * sizeof(double), 1, binfile)) {
          goto king_cutoff_batch_ret_READ_FAIL;
        }
        const uintptr_t sample_idx = king_uidx_to_sample_idx[king_uidx];
        uintptr_t* kinship_table_row = &(kinship_table[sample_idx * orig_sample_ctl]);
        uintptr_t* kinship_table_col = &(kinship_table[sample_idx / kBitsPerWord]);
        const uintptr_t kinship_new_bit = k1LU << (sample_idx % kBitsPerWord);
        uint32_t king_uidx2 = first_king_uidx;
        for (uint32_t king_idx2 = 0; king_idx2 < king_idx; ++king_idx2, ++king_uidx2) {
          next_set_unsafe_ck(king_include, &king_uidx2);
          if (king_drow[king_uidx2] > king_cutoff) {
            const uintptr_t sample_idx2 = king_uidx_to_sample_idx[king_uidx2];
            SET_BIT(sample_idx2, kinship_table_row);
            kinship_table_col[sample_idx2 * orig_sample_ctl] |= kinship_new_bit;
            ++constraint_ct;
          }
        }
      }
    } else {
      if (fsize != (fsize_double_expected / 2)) {
        LOGERRPRINTFWW("Error: Invalid --king-cutoff .bin file size (expected %" PRIu64 " or %" PRIu64 " bytes).\n", fsize_double_expected / 2, fsize_double_expected);
        goto king_cutoff_batch_ret_MALFORMED_INPUT;
      }
      assert(king_id_ct <= ((0x7ffff000 / sizeof(float)) + 1));
      const float king_cutoff_f = S_CAST(float, king_cutoff);
      float* king_frow;
      if (bigstack_alloc_f(king_id_ct - 1, &king_frow)) {
        goto king_cutoff_batch_ret_NOMEM;
      }
      for (uint32_t king_idx = 1; king_uidx < king_id_ct; ++king_idx, ++king_uidx) {
        if (!IS_SET(king_include, king_uidx)) {
          king_uidx = next_set(king_include, king_uidx + 1, king_id_ct);
          if (king_uidx == king_id_ct) {
            break;
          }
          if (fseeko(binfile, S_CAST(uint64_t, king_uidx) * (king_uidx - 1) * (sizeof(float) / 2), SEEK_SET)) {
            goto king_cutoff_batch_ret_READ_FAIL;
          }
        }
        if (!fread_unlocked(king_frow, king_uidx * sizeof(float), 1, binfile)) {
          goto king_cutoff_batch_ret_READ_FAIL;
        }
        const uintptr_t sample_idx = king_uidx_to_sample_idx[king_uidx];
        uintptr_t* kinship_table_row = &(kinship_table[sample_idx * orig_sample_ctl]);
        uintptr_t* kinship_table_col = &(kinship_table[sample_idx / kBitsPerWord]);
        const uintptr_t kinship_new_bit = k1LU << (sample_idx % kBitsPerWord);
        uint32_t king_uidx2 = first_king_uidx;
        for (uint32_t king_idx2 = 0; king_idx2 < king_idx; ++king_idx2, ++king_uidx2) {
          next_set_unsafe_ck(king_include, &king_uidx2);
          if (king_frow[king_uidx2] > king_cutoff_f) {
            const uintptr_t sample_idx2 = king_uidx_to_sample_idx[king_uidx2];
            SET_BIT(sample_idx2, kinship_table_row);
            kinship_table_col[sample_idx2 * orig_sample_ctl] |= kinship_new_bit;
            ++constraint_ct;
          }
        }
      }
    }
    LOGPRINTF("--king-cutoff: %" PRIuPTR " constraint%s loaded.\n", constraint_ct, (constraint_ct == 1)? "" : "s");
    bigstack_reset(sample_uidx_to_king_uidx);
    if (kinship_prune_destructive(kinship_table, sample_include, sample_ct_ptr)) {
      goto king_cutoff_batch_ret_NOMEM;
    }
  }
  while (0) {
  king_cutoff_batch_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  king_cutoff_batch_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  king_cutoff_batch_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  king_cutoff_batch_ret_MISSING_TOKENS:
    LOGERRPRINTFWW("Error: Fewer tokens than expected on line %" PRIuPTR " of %s .\n", line_idx, king_cutoff_fprefix);
    break;
  king_cutoff_batch_ret_LONG_LINE:
    snprintf(g_logbuf, kLogbufSize, "Error: Line %" PRIuPTR " of %s is pathologically long.\n", line_idx, king_cutoff_fprefix);
  king_cutoff_batch_ret_MALFORMED_INPUT_WW:
    wordwrapb(0);
    logerrprintb();
  king_cutoff_batch_ret_MALFORMED_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  }
 king_cutoff_batch_ret_1:
  fclose_cond(binfile);
  gzclose_cond(gz_infile);
  bigstack_reset(bigstack_mark);
  return reterr;
}

// multithread globals
static uintptr_t* g_smaj_hom[2] = {nullptr, nullptr};
static uintptr_t* g_smaj_ref2het[2] = {nullptr, nullptr};
static uint32_t* g_thread_start = nullptr;
static uint32_t* g_king_counts = nullptr;
static uint32_t* g_loaded_sample_idx_pairs = nullptr;
static uint32_t g_homhom_needed = 0;

#ifdef USE_SSE42
CONSTU31(kKingMultiplex, 1024);
CONSTU31(kKingMultiplexWords, kKingMultiplex / kBitsPerWord);
void incr_king(const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts_iter) {
  // Tried adding another level of blocking, but couldn't get it to make a
  // difference.
  for (uint32_t second_idx = start_idx; second_idx < end_idx; ++second_idx) {
    // technically overflows for huge sample_ct
    const uint32_t second_offset = second_idx * kKingMultiplexWords;
    const uintptr_t* second_hom = &(smaj_hom[second_offset]);
    const uintptr_t* second_ref2het = &(smaj_ref2het[second_offset]);
    const uintptr_t* first_hom_iter = smaj_hom;
    const uintptr_t* first_ref2het_iter = smaj_ref2het;
    while (first_hom_iter < second_hom) {
      uint32_t acc_ibs0 = 0;
      uint32_t acc_hethet = 0;
      uint32_t acc_het2hom1 = 0;
      uint32_t acc_het1hom2 = 0;
      for (uint32_t widx = 0; widx < kKingMultiplexWords; ++widx) {
        const uintptr_t hom1 = first_hom_iter[widx];
        const uintptr_t hom2 = second_hom[widx];
        const uintptr_t ref2het1 = first_ref2het_iter[widx];
        const uintptr_t ref2het2 = second_ref2het[widx];
        const uintptr_t homhom = hom1 & hom2;
        const uintptr_t het1 = ref2het1 & (~hom1);
        const uintptr_t het2 = ref2het2 & (~hom2);
        acc_ibs0 += popcount_long((ref2het1 ^ ref2het2) & homhom);
        acc_hethet += popcount_long(het1 & het2);
        acc_het2hom1 += popcount_long(hom1 & het2);
        acc_het1hom2 += popcount_long(hom2 & het1);
      }
      *king_counts_iter++ += acc_ibs0;
      *king_counts_iter++ += acc_hethet;
      *king_counts_iter++ += acc_het2hom1;
      *king_counts_iter++ += acc_het1hom2;

      first_hom_iter = &(first_hom_iter[kKingMultiplexWords]);
      first_ref2het_iter = &(first_ref2het_iter[kKingMultiplexWords]);
    }
  }
}

void incr_king_homhom(const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts_iter) {
  for (uint32_t second_idx = start_idx; second_idx < end_idx; ++second_idx) {
    // technically overflows for huge sample_ct
    const uint32_t second_offset = second_idx * kKingMultiplexWords;
    const uintptr_t* second_hom = &(smaj_hom[second_offset]);
    const uintptr_t* second_ref2het = &(smaj_ref2het[second_offset]);
    const uintptr_t* first_hom_iter = smaj_hom;
    const uintptr_t* first_ref2het_iter = smaj_ref2het;
    while (first_hom_iter < second_hom) {
      uint32_t acc_homhom = 0;
      uint32_t acc_ibs0 = 0;
      uint32_t acc_hethet = 0;
      uint32_t acc_het2hom1 = 0;
      uint32_t acc_het1hom2 = 0;
      for (uint32_t widx = 0; widx < kKingMultiplexWords; ++widx) {
        const uintptr_t hom1 = first_hom_iter[widx];
        const uintptr_t hom2 = second_hom[widx];
        const uintptr_t ref2het1 = first_ref2het_iter[widx];
        const uintptr_t ref2het2 = second_ref2het[widx];
        const uintptr_t homhom = hom1 & hom2;
        const uintptr_t het1 = ref2het1 & (~hom1);
        const uintptr_t het2 = ref2het2 & (~hom2);
        acc_homhom += popcount_long(homhom);
        acc_ibs0 += popcount_long((ref2het1 ^ ref2het2) & homhom);
        acc_hethet += popcount_long(het1 & het2);
        acc_het2hom1 += popcount_long(hom1 & het2);
        acc_het1hom2 += popcount_long(hom2 & het1);
      }
      *king_counts_iter++ += acc_ibs0;
      *king_counts_iter++ += acc_hethet;
      *king_counts_iter++ += acc_het2hom1;
      *king_counts_iter++ += acc_het1hom2;
      *king_counts_iter++ += acc_homhom;

      first_hom_iter = &(first_hom_iter[kKingMultiplexWords]);
      first_ref2het_iter = &(first_ref2het_iter[kKingMultiplexWords]);
    }
  }
}
#else // !USE_SSE42
#  ifdef __LP64__
CONSTU31(kKingMultiplex, 1536);
#  else
CONSTU31(kKingMultiplex, 960);
#  endif
static_assert(kKingMultiplex % (3 * kBitsPerVec) == 0, "Invalid kKingMultiplex value.");
CONSTU31(kKingMultiplexWords, kKingMultiplex / kBitsPerWord);
CONSTU31(kKingMultiplexVecs, kKingMultiplex / kBitsPerVec);
// expensive popcount_long().  Use Lauradoux/Walisch accumulators, since
// Harley-Seal requires too many variables.
void incr_king(const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts_iter) {
  const vul_t m1 = VCONST_UL(kMask5555);
  const vul_t m2 = VCONST_UL(kMask3333);
  const vul_t m4 = VCONST_UL(kMask0F0F);
  for (uint32_t second_idx = start_idx; second_idx < end_idx; ++second_idx) {
    // technically overflows for huge sample_ct
    const uint32_t second_offset = second_idx * kKingMultiplexWords;
    const vul_t* second_hom = R_CAST(const vul_t*, &(smaj_hom[second_offset]));
    const vul_t* second_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[second_offset]));
    const vul_t* first_hom_iter = R_CAST(const vul_t*, smaj_hom);
    const vul_t* first_ref2het_iter = R_CAST(const vul_t*, smaj_ref2het);
    while (first_hom_iter < second_hom) {
      univec_t acc_ibs0;
      univec_t acc_hethet;
      univec_t acc_het2hom1;
      univec_t acc_het1hom2;
      acc_ibs0.vi = vul_setzero();
      acc_hethet.vi = vul_setzero();
      acc_het2hom1.vi = vul_setzero();
      acc_het1hom2.vi = vul_setzero();
      for (uint32_t vec_idx = 0; vec_idx < kKingMultiplexVecs; vec_idx += 3) {
        vul_t hom1 = first_hom_iter[vec_idx];
        vul_t hom2 = second_hom[vec_idx];
        vul_t ref2het1 = first_ref2het_iter[vec_idx];
        vul_t ref2het2 = second_ref2het[vec_idx];
        vul_t het1 = ref2het1 & (~hom1);
        vul_t het2 = ref2het2 & (~hom2);
        vul_t agg_ibs0 = (ref2het1 ^ ref2het2) & (hom1 & hom2);
        vul_t agg_hethet = het1 & het2;
        vul_t agg_het2hom1 = hom1 & het2;
        vul_t agg_het1hom2 = hom2 & het1;
        agg_ibs0 = agg_ibs0 - (vul_rshift(agg_ibs0, 1) & m1);
        agg_hethet = agg_hethet - (vul_rshift(agg_hethet, 1) & m1);
        agg_het2hom1 = agg_het2hom1 - (vul_rshift(agg_het2hom1, 1) & m1);
        agg_het1hom2 = agg_het1hom2 - (vul_rshift(agg_het1hom2, 1) & m1);
        agg_ibs0 = (agg_ibs0 & m2) + (vul_rshift(agg_ibs0, 2) & m2);
        agg_hethet = (agg_hethet & m2) + (vul_rshift(agg_hethet, 2) & m2);
        agg_het2hom1 = (agg_het2hom1 & m2) + (vul_rshift(agg_het2hom1, 2) & m2);
        agg_het1hom2 = (agg_het1hom2 & m2) + (vul_rshift(agg_het1hom2, 2) & m2);

        for (uint32_t offset = 1; offset < 3; ++offset) {
          hom1 = first_hom_iter[vec_idx + offset];
          hom2 = second_hom[vec_idx + offset];
          ref2het1 = first_ref2het_iter[vec_idx + offset];
          ref2het2 = second_ref2het[vec_idx + offset];
          het1 = ref2het1 & (~hom1);
          het2 = ref2het2 & (~hom2);
          vul_t cur_ibs0 = (ref2het1 ^ ref2het2) & (hom1 & hom2);
          vul_t cur_hethet = het1 & het2;
          vul_t cur_het2hom1 = hom1 & het2;
          vul_t cur_het1hom2 = hom2 & het1;
          cur_ibs0 = cur_ibs0 - (vul_rshift(cur_ibs0, 1) & m1);
          cur_hethet = cur_hethet - (vul_rshift(cur_hethet, 1) & m1);
          cur_het2hom1 = cur_het2hom1 - (vul_rshift(cur_het2hom1, 1) & m1);
          cur_het1hom2 = cur_het1hom2 - (vul_rshift(cur_het1hom2, 1) & m1);
          agg_ibs0 += (cur_ibs0 & m2) + (vul_rshift(cur_ibs0, 2) & m2);
          agg_hethet += (cur_hethet & m2) + (vul_rshift(cur_hethet, 2) & m2);
          agg_het2hom1 += (cur_het2hom1 & m2) + (vul_rshift(cur_het2hom1, 2) & m2);
          agg_het1hom2 += (cur_het1hom2 & m2) + (vul_rshift(cur_het1hom2, 2) & m2);
        }
        acc_ibs0.vi = acc_ibs0.vi + (agg_ibs0 & m4) + (vul_rshift(agg_ibs0, 4) & m4);
        acc_hethet.vi = acc_hethet.vi + (agg_hethet & m4) + (vul_rshift(agg_hethet, 4) & m4);
        acc_het2hom1.vi = acc_het2hom1.vi + (agg_het2hom1 & m4) + (vul_rshift(agg_het2hom1, 4) & m4);
        acc_het1hom2.vi = acc_het1hom2.vi + (agg_het1hom2 & m4) + (vul_rshift(agg_het1hom2, 4) & m4);
      }
      const vul_t m8 = VCONST_UL(kMask00FF);
      acc_ibs0.vi = (acc_ibs0.vi & m8) + (vul_rshift(acc_ibs0.vi, 8) & m8);
      acc_hethet.vi = (acc_hethet.vi & m8) + (vul_rshift(acc_hethet.vi, 8) & m8);
      acc_het2hom1.vi = (acc_het2hom1.vi & m8) + (vul_rshift(acc_het2hom1.vi, 8) & m8);
      acc_het1hom2.vi = (acc_het1hom2.vi & m8) + (vul_rshift(acc_het1hom2.vi, 8) & m8);
      *king_counts_iter++ += univec_hsum_16bit(acc_ibs0);
      *king_counts_iter++ += univec_hsum_16bit(acc_hethet);
      *king_counts_iter++ += univec_hsum_16bit(acc_het2hom1);
      *king_counts_iter++ += univec_hsum_16bit(acc_het1hom2);

      first_hom_iter = &(first_hom_iter[kKingMultiplexVecs]);
      first_ref2het_iter = &(first_ref2het_iter[kKingMultiplexVecs]);
    }
  }
}

void incr_king_homhom(const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts_iter) {
  const vul_t m1 = VCONST_UL(kMask5555);
  const vul_t m2 = VCONST_UL(kMask3333);
  const vul_t m4 = VCONST_UL(kMask0F0F);
  for (uint32_t second_idx = start_idx; second_idx < end_idx; ++second_idx) {
    // technically overflows for huge sample_ct
    const uint32_t second_offset = second_idx * kKingMultiplexWords;
    const vul_t* second_hom = R_CAST(const vul_t*, &(smaj_hom[second_offset]));
    const vul_t* second_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[second_offset]));
    const vul_t* first_hom_iter = R_CAST(const vul_t*, smaj_hom);
    const vul_t* first_ref2het_iter = R_CAST(const vul_t*, smaj_ref2het);
    while (first_hom_iter < second_hom) {
      univec_t acc_homhom;
      univec_t acc_ibs0;
      univec_t acc_hethet;
      univec_t acc_het2hom1;
      univec_t acc_het1hom2;
      acc_homhom.vi = vul_setzero();
      acc_ibs0.vi = vul_setzero();
      acc_hethet.vi = vul_setzero();
      acc_het2hom1.vi = vul_setzero();
      acc_het1hom2.vi = vul_setzero();
      for (uint32_t vec_idx = 0; vec_idx < kKingMultiplexVecs; vec_idx += 3) {
        vul_t hom1 = first_hom_iter[vec_idx];
        vul_t hom2 = second_hom[vec_idx];
        vul_t ref2het1 = first_ref2het_iter[vec_idx];
        vul_t ref2het2 = second_ref2het[vec_idx];
        vul_t agg_homhom = hom1 & hom2;
        vul_t het1 = ref2het1 & (~hom1);
        vul_t het2 = ref2het2 & (~hom2);
        vul_t agg_ibs0 = (ref2het1 ^ ref2het2) & agg_homhom;
        vul_t agg_hethet = het1 & het2;
        vul_t agg_het2hom1 = hom1 & het2;
        vul_t agg_het1hom2 = hom2 & het1;
        agg_homhom = agg_homhom - (vul_rshift(agg_homhom, 1) & m1);
        agg_ibs0 = agg_ibs0 - (vul_rshift(agg_ibs0, 1) & m1);
        agg_hethet = agg_hethet - (vul_rshift(agg_hethet, 1) & m1);
        agg_het2hom1 = agg_het2hom1 - (vul_rshift(agg_het2hom1, 1) & m1);
        agg_het1hom2 = agg_het1hom2 - (vul_rshift(agg_het1hom2, 1) & m1);
        agg_homhom = (agg_homhom & m2) + (vul_rshift(agg_homhom, 2) & m2);
        agg_ibs0 = (agg_ibs0 & m2) + (vul_rshift(agg_ibs0, 2) & m2);
        agg_hethet = (agg_hethet & m2) + (vul_rshift(agg_hethet, 2) & m2);
        agg_het2hom1 = (agg_het2hom1 & m2) + (vul_rshift(agg_het2hom1, 2) & m2);
        agg_het1hom2 = (agg_het1hom2 & m2) + (vul_rshift(agg_het1hom2, 2) & m2);

        for (uint32_t offset = 1; offset < 3; ++offset) {
          hom1 = first_hom_iter[vec_idx + offset];
          hom2 = second_hom[vec_idx + offset];
          ref2het1 = first_ref2het_iter[vec_idx + offset];
          ref2het2 = second_ref2het[vec_idx + offset];
          vul_t cur_homhom = hom1 & hom2;
          het1 = ref2het1 & (~hom1);
          het2 = ref2het2 & (~hom2);
          vul_t cur_ibs0 = (ref2het1 ^ ref2het2) & cur_homhom;
          vul_t cur_hethet = het1 & het2;
          vul_t cur_het2hom1 = hom1 & het2;
          vul_t cur_het1hom2 = hom2 & het1;
          cur_homhom = cur_homhom - (vul_rshift(cur_homhom, 1) & m1);
          cur_ibs0 = cur_ibs0 - (vul_rshift(cur_ibs0, 1) & m1);
          cur_hethet = cur_hethet - (vul_rshift(cur_hethet, 1) & m1);
          cur_het2hom1 = cur_het2hom1 - (vul_rshift(cur_het2hom1, 1) & m1);
          cur_het1hom2 = cur_het1hom2 - (vul_rshift(cur_het1hom2, 1) & m1);
          agg_homhom += (cur_homhom & m2) + (vul_rshift(cur_homhom, 2) & m2);
          agg_ibs0 += (cur_ibs0 & m2) + (vul_rshift(cur_ibs0, 2) & m2);
          agg_hethet += (cur_hethet & m2) + (vul_rshift(cur_hethet, 2) & m2);
          agg_het2hom1 += (cur_het2hom1 & m2) + (vul_rshift(cur_het2hom1, 2) & m2);
          agg_het1hom2 += (cur_het1hom2 & m2) + (vul_rshift(cur_het1hom2, 2) & m2);
        }
        acc_homhom.vi = acc_homhom.vi + (agg_homhom & m4) + (vul_rshift(agg_homhom, 4) & m4);
        acc_ibs0.vi = acc_ibs0.vi + (agg_ibs0 & m4) + (vul_rshift(agg_ibs0, 4) & m4);
        acc_hethet.vi = acc_hethet.vi + (agg_hethet & m4) + (vul_rshift(agg_hethet, 4) & m4);
        acc_het2hom1.vi = acc_het2hom1.vi + (agg_het2hom1 & m4) + (vul_rshift(agg_het2hom1, 4) & m4);
        acc_het1hom2.vi = acc_het1hom2.vi + (agg_het1hom2 & m4) + (vul_rshift(agg_het1hom2, 4) & m4);
      }
      const vul_t m8 = VCONST_UL(kMask00FF);
      acc_homhom.vi = (acc_homhom.vi & m8) + (vul_rshift(acc_homhom.vi, 8) & m8);
      acc_ibs0.vi = (acc_ibs0.vi & m8) + (vul_rshift(acc_ibs0.vi, 8) & m8);
      acc_hethet.vi = (acc_hethet.vi & m8) + (vul_rshift(acc_hethet.vi, 8) & m8);
      acc_het2hom1.vi = (acc_het2hom1.vi & m8) + (vul_rshift(acc_het2hom1.vi, 8) & m8);
      acc_het1hom2.vi = (acc_het1hom2.vi & m8) + (vul_rshift(acc_het1hom2.vi, 8) & m8);
      *king_counts_iter++ += univec_hsum_16bit(acc_ibs0);
      *king_counts_iter++ += univec_hsum_16bit(acc_hethet);
      *king_counts_iter++ += univec_hsum_16bit(acc_het2hom1);
      *king_counts_iter++ += univec_hsum_16bit(acc_het1hom2);
      *king_counts_iter++ += univec_hsum_16bit(acc_homhom);

      first_hom_iter = &(first_hom_iter[kKingMultiplexVecs]);
      first_ref2het_iter = &(first_ref2het_iter[kKingMultiplexVecs]);
    }
  }
}
#endif
static_assert(!(kKingMultiplexWords % 2), "kKingMultiplexWords must be even for safe bit-transpose.");

THREAD_FUNC_DECL calc_king_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint64_t mem_start_idx = g_thread_start[0];
  const uint64_t start_idx = g_thread_start[tidx];
  const uint32_t end_idx = g_thread_start[tidx + 1];
  const uint32_t homhom_needed = g_homhom_needed;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    if (homhom_needed) {
      incr_king_homhom(g_smaj_hom[parity], g_smaj_ref2het[parity], start_idx, end_idx, &(g_king_counts[((start_idx * (start_idx - 1) - mem_start_idx * (mem_start_idx - 1)) / 2) * 5]));
    } else {
      incr_king(g_smaj_hom[parity], g_smaj_ref2het[parity], start_idx, end_idx, &(g_king_counts[(start_idx * (start_idx - 1) - mem_start_idx * (mem_start_idx - 1)) * 2]));
    }
    if (is_last_block) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

double compute_kinship(const uint32_t* king_counts_entry) {
  const uint32_t ibs0_ct = king_counts_entry[0];
  const uint32_t hethet_ct = king_counts_entry[1];
  const uint32_t het2hom1_ct = king_counts_entry[2];
  const uint32_t het1hom2_ct = king_counts_entry[3];
  // const uint32_t homhom_ct = king_counts_entry[4];
  const intptr_t smaller_het_ct = hethet_ct + MINV(het1hom2_ct, het2hom1_ct);
  return 0.5 - (S_CAST(double, 4 * S_CAST(intptr_t, ibs0_ct) + het1hom2_ct + het2hom1_ct) / S_CAST(double, 4 * smaller_het_ct));
}

// could also return pointer to end?
void set_king_matrix_fname(king_flags_t king_modifier, uint32_t parallel_idx, uint32_t parallel_tot, char* outname_end) {
  if (!(king_modifier & (kfKingMatrixBin | kfKingMatrixBin4))) {
    char* outname_end2 = strcpya(outname_end, ".king");
    const uint32_t output_zst = king_modifier & kfKingMatrixZs;
    if (parallel_tot != 1) {
      *outname_end2++ = '.';
      outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
    }
    if (output_zst) {
      outname_end2 = strcpya(outname_end2, ".zst");
    }
    *outname_end2 = '\0';
    return;
  }
  char* outname_end2 = strcpya(outname_end, ".king.bin");
  if (parallel_tot != 1) {
    *outname_end2++ = '.';
    outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
  }
  *outname_end2 = '\0';
}

void set_king_table_fname(king_flags_t king_modifier, uint32_t parallel_idx, uint32_t parallel_tot, char* outname_end) {
  char* outname_end2 = strcpya(outname_end, ".kin0");
  const uint32_t output_zst = king_modifier & kfKingTableZs;
  if (parallel_tot != 1) {
    *outname_end2++ = '.';
    outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
  }
  if (output_zst) {
    outname_end2 = strcpya(outname_end2, ".zst");
  }
  *outname_end2 = '\0';
}

char* append_king_table_header(king_flags_t king_modifier, uint32_t king_col_sid, char* cswritep) {
  *cswritep++ = '#';
  if (king_modifier & kfKingColId) {
    cswritep = strcpya(cswritep, "FID1\tID1\t");
    if (king_col_sid) {
      cswritep = strcpya(cswritep, "SID1\tFID2\tID2\tSID2\t");
    } else {
      cswritep = strcpya(cswritep, "FID2\tID2\t");
    }
  }
  if (king_modifier & kfKingColNsnp) {
    cswritep = strcpya(cswritep, "NSNP\t");
  }
  if (king_modifier & kfKingColHethet) {
    cswritep = strcpya(cswritep, "HETHET\t");
  }
  if (king_modifier & kfKingColIbs0) {
    cswritep = strcpya(cswritep, "IBS0\t");
  }
  if (king_modifier & kfKingColIbs1) {
    cswritep = strcpya(cswritep, "HET1_HOM2\tHET2_HOM1\t");
  }
  if (king_modifier & kfKingColKinship) {
    cswritep = strcpya(cswritep, "KINSHIP\t");
  }
  decr_append_binary_eoln(&cswritep);
  return cswritep;
}

pglerr_t calc_king(const char* sample_ids, const char* sids, uintptr_t* variant_include, const chr_info_t* cip, uint32_t raw_sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, uint32_t raw_variant_ct, uint32_t variant_ct, double king_cutoff, double king_table_filter, king_flags_t king_modifier, uint32_t parallel_idx, uint32_t parallel_tot, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, uintptr_t* sample_include, uint32_t* sample_ct_ptr, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  char* cswritep = nullptr;
  char* cswritetp = nullptr;
  compress_stream_state_t css;
  compress_stream_state_t csst;
  threads_state_t ts;
  pglerr_t reterr = kPglRetSuccess;
  cswrite_init_null(&css);
  cswrite_init_null(&csst);
  init_threads3z(&ts);
  {
    const king_flags_t matrix_shape = king_modifier & kfKingMatrixShapemask;
    const char* flagname = matrix_shape? "--make-king" : ((king_modifier & kfKingColAll)? "--make-king-table" : "--king-cutoff");
    if (is_set(cip->haploid_mask, 0)) {
      LOGERRPRINTF("Error: %s cannot be used on haploid genomes.\n", flagname);
      goto calc_king_ret_INCONSISTENT_INPUT;
    }
    reterr = conditional_allocate_non_autosomal_variants(cip, "KING-robust calculation", raw_variant_ct, &variant_include, &variant_ct);
    if (reterr) {
      goto calc_king_ret_1;
    }
    uint32_t sample_ct = *sample_ct_ptr;
    if (sample_ct < 2) {
      LOGERRPRINTF("Error: %s requires at least 2 samples.\n", flagname);
      goto calc_king_ret_INCONSISTENT_INPUT;
    }
#ifdef __LP64__
    // there's also a UINT32_MAX / kKingMultiplexWords limit, but that's not
    // relevant for now
    if (sample_ct > 134000000) {
      // for text output, 134m * 16 is just below kMaxLongLine
      LOGERRPRINTF("Error: %s does not support > 134000000 samples.\n", flagname);
      reterr = kPglRetNotYetSupported;
      goto calc_king_ret_1;
    }
#endif
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    if (calc_thread_ct > sample_ct / 32) {
      calc_thread_ct = sample_ct / 32;
    }
    if (!calc_thread_ct) {
      calc_thread_ct = 1;
    }
    // possible todo: allow this to change between passes
    ts.calc_thread_ct = calc_thread_ct;
    if (bigstack_alloc_ui(calc_thread_ct + 1, &g_thread_start) ||
        bigstack_alloc_thread(calc_thread_ct, &ts.threads)) {
      goto calc_king_ret_NOMEM;
    }
    const uintptr_t sample_ctl = BITCT_TO_WORDCT(sample_ct);
    uintptr_t* kinship_table = nullptr;
    if (king_cutoff != -1) {
      if (bigstack_calloc_ul(sample_ct * sample_ctl, &kinship_table)) {
        goto calc_king_ret_NOMEM;
      }
    }
    const uint32_t raw_sample_ctl = BITCT_TO_WORDCT(raw_sample_ct);
    const uint32_t sample_ctaw = BITCT_TO_ALIGNED_WORDCT(sample_ct);
    const uint32_t sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(sample_ct);
    g_homhom_needed = (king_modifier & kfKingColNsnp) || ((!(king_modifier & kfKingCounts)) && (king_modifier & (kfKingColHethet | kfKingColIbs0 | kfKingColIbs1)));
    uint32_t grand_row_start_idx;
    uint32_t grand_row_end_idx;
    parallel_bounds(sample_ct, 1, parallel_idx, parallel_tot, R_CAST(int32_t*, &grand_row_start_idx), R_CAST(int32_t*, &grand_row_end_idx));
    const uint32_t king_bufsizew = kKingMultiplexWords * grand_row_end_idx;
    const uint32_t homhom_needed_p4 = g_homhom_needed + 4;
    uintptr_t* cur_sample_include;
    uint32_t* sample_include_cumulative_popcounts;
    uintptr_t* loadbuf;
    uintptr_t* splitbuf_hom;
    uintptr_t* splitbuf_ref2het;
    if (bigstack_alloc_ul(raw_sample_ctl, &cur_sample_include) ||
        bigstack_alloc_ui(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_ul(sample_ctaw2, &loadbuf) ||
        bigstack_alloc_ul(kPglBitTransposeBatch * sample_ctaw, &splitbuf_hom) ||
        bigstack_alloc_ul(kPglBitTransposeBatch * sample_ctaw, &splitbuf_ref2het) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_hom[0])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_ref2het[0])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_hom[1])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_ref2het[1]))) {
      goto calc_king_ret_NOMEM;
    }
    // force this to be cacheline-aligned
    vul_t* vecaligned_buf = S_CAST(vul_t*, bigstack_alloc(kPglBitTransposeBufbytes));
    if (!vecaligned_buf) {
      goto calc_king_ret_NOMEM;
    }

    // Make this automatically multipass when there's insufficient memory.  So
    // we open the output file(s) here, and just append in the main loop.
    unsigned char* numbuf = nullptr;
    if (matrix_shape) {
      set_king_matrix_fname(king_modifier, parallel_idx, parallel_tot, outname_end);
      if (!(king_modifier & (kfKingMatrixBin | kfKingMatrixBin4))) {
        // text matrix
        // won't be >2gb since sample_ct <= 134m
        const uint32_t overflow_buf_size = kCompressStreamBlock + 16 * sample_ct;
        reterr = cswrite_init2(outname, 0, king_modifier & kfKingMatrixZs, max_thread_ct, overflow_buf_size, &css, &cswritep);
        if (reterr) {
          goto calc_king_ret_1;
        }
      } else {
        if (fopen_checked(outname, FOPEN_WB, &outfile)) {
          goto calc_king_ret_OPEN_FAIL;
        }
        if (bigstack_alloc_uc(sample_ct * 4 * (2 - ((king_modifier / kfKingMatrixBin4) & 1)), &numbuf)) {
          goto calc_king_ret_OPEN_FAIL;
        }
      }
    }
    uint32_t king_col_sid = 0;
    uintptr_t max_sample_augid_blen = max_sample_id_blen;
    char* collapsed_sample_augids = nullptr;
    if (king_modifier & kfKingColAll) {
      const uint32_t overflow_buf_size = kCompressStreamBlock + kMaxMediumLine;
      set_king_table_fname(king_modifier, parallel_idx, parallel_tot, outname_end);
      reterr = cswrite_init2(outname, 0, king_modifier & kfKingTableZs, max_thread_ct, overflow_buf_size, &csst, &cswritetp);
      if (reterr) {
        goto calc_king_ret_1;
      }

      king_col_sid = sid_col_required(sample_include, sids, sample_ct, max_sid_blen, king_modifier / kfKingColMaybesid);
      if (!parallel_idx) {
        cswritetp = append_king_table_header(king_modifier, king_col_sid, cswritetp);
      }
      if (king_col_sid) {
        if (augid_init_alloc(sample_include, sample_ids, sids, grand_row_end_idx, max_sample_id_blen, max_sid_blen, nullptr, &collapsed_sample_augids, &max_sample_augid_blen)) {
          goto calc_king_ret_NOMEM;
        }
      } else {
        if (bigstack_alloc_c(grand_row_end_idx * max_sample_augid_blen, &collapsed_sample_augids)) {
          goto calc_king_ret_NOMEM;
        }
        uint32_t sample_uidx = 0;
        for (uint32_t sample_idx = 0; sample_idx < grand_row_end_idx; ++sample_idx, ++sample_uidx) {
          next_set_unsafe_ck(sample_include, &sample_uidx);
          strcpy(&(collapsed_sample_augids[sample_idx * max_sample_augid_blen]), &(sample_ids[sample_uidx * max_sample_id_blen]));
        }
      }
    }
    uint64_t king_table_filter_ct = 0;
    const uintptr_t cells_avail = bigstack_left() / (sizeof(int32_t) * homhom_needed_p4);
    const uint32_t pass_ct = count_triangle_passes(grand_row_start_idx, grand_row_end_idx, 1, cells_avail);
    if (!pass_ct) {
      goto calc_king_ret_NOMEM;
    }
    if ((pass_ct > 1) && (king_modifier & kfKingMatrixSq)) {
      logerrprint("Insufficient memory for --make-king square output.  Try square0 or triangle\nshape instead.\n");
      goto calc_king_ret_NOMEM;
    }
    uint32_t row_end_idx = grand_row_start_idx;
    g_king_counts = R_CAST(uint32_t*, g_bigstack_base);
    for (uint32_t pass_idx_p1 = 1; pass_idx_p1 <= pass_ct; ++pass_idx_p1) {
      const uint32_t row_start_idx = row_end_idx;
      row_end_idx = next_triangle_pass(row_start_idx, grand_row_end_idx, 1, cells_avail);
      triangle_load_balance(calc_thread_ct, row_start_idx, row_end_idx, 1, g_thread_start);
      const uintptr_t tot_cells = (S_CAST(uint64_t, row_end_idx) * (row_end_idx - 1) - S_CAST(uint64_t, row_start_idx) * (row_start_idx - 1)) / 2;
      fill_uint_zero(tot_cells * homhom_needed_p4, g_king_counts);

      const uint32_t row_end_idxaw = BITCT_TO_ALIGNED_WORDCT(row_end_idx);
      const uint32_t row_end_idxaw2 = QUATERCT_TO_ALIGNED_WORDCT(row_end_idx);
      if (row_end_idxaw % 2) {
        const uint32_t cur_king_bufsizew = kKingMultiplexWords * row_end_idx;
        uintptr_t* smaj_hom0_last = &(g_smaj_hom[0][kKingMultiplexWords - 1]);
        uintptr_t* smaj_ref2het0_last = &(g_smaj_ref2het[0][kKingMultiplexWords - 1]);
        uintptr_t* smaj_hom1_last = &(g_smaj_hom[1][kKingMultiplexWords - 1]);
        uintptr_t* smaj_ref2het1_last = &(g_smaj_ref2het[1][kKingMultiplexWords - 1]);
        for (uint32_t offset = 0; offset < cur_king_bufsizew; offset += kKingMultiplexWords) {
          smaj_hom0_last[offset] = 0;
          smaj_ref2het0_last[offset] = 0;
          smaj_hom1_last[offset] = 0;
          smaj_ref2het1_last[offset] = 0;
        }
      }
      memcpy(cur_sample_include, sample_include, raw_sample_ctl * sizeof(intptr_t));
      if (row_end_idx != grand_row_end_idx) {
        uint32_t sample_uidx_end = idx_to_uidx_basic(sample_include, row_end_idx);
        clear_bits_nz(sample_uidx_end, raw_sample_ct, cur_sample_include);
      }
      fill_cumulative_popcounts(cur_sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
      if (pass_idx_p1 != 1) {
        reinit_threads3z(&ts);
      }
      uint32_t variant_uidx = 0;
      uint32_t variants_completed = 0;
      uint32_t parity = 0;
      const uint32_t sample_batch_ct_m1 = (row_end_idx - 1) / kPglBitTransposeBatch;
      // Similar to plink 1.9 --genome.  For each pair of samples S1-S2, we
      // need to determine counts of the following:
      //   * S1 hom-S2 opposite hom
      //   * het-het
      //   * S1 hom-S2 het
      //   * S2 hom-S1 het
      //   * sometimes S1 hom-S2 same hom
      //   * (nonmissing determined via subtraction)
      // We handle this as follows:
      //   1. set n=0, reader thread loads first kKingMultiplex variants and
      //      converts+transposes the data to a sample-major format suitable
      //      for multithreaded computation.
      //   2. spawn threads
      //
      //   3. increment n by 1
      //   4. load block n unless eof
      //   5. permit threads to continue to next block, unless eof
      //   6. goto step 3 unless eof
      //
      //   7. write results
      // Results are always reported in lower-triangular order, rather than
      // KING's upper-triangular order, since the former plays more nicely with
      // incremental addition of samples.
      pgr_clear_ld_cache(simple_pgrp);
      do {
        const uint32_t cur_block_size = MINV(variant_ct - variants_completed, kKingMultiplex);
        uintptr_t* cur_smaj_hom = g_smaj_hom[parity];
        uintptr_t* cur_smaj_ref2het = g_smaj_ref2het[parity];
        uint32_t write_batch_idx = 0;
        // "block" = distance computation granularity, usually 1024 or 1536
        //           variants
        // "batch" = variant-major-to-sample-major transpose granularity,
        //           currently 512 variants
        uint32_t variant_batch_size = kPglBitTransposeBatch;
        uint32_t variant_batch_size_rounded_up = kPglBitTransposeBatch;
        const uint32_t write_batch_ct_m1 = (cur_block_size - 1) / kPglBitTransposeBatch;
        while (1) {
          if (write_batch_idx >= write_batch_ct_m1) {
            if (write_batch_idx > write_batch_ct_m1) {
              break;
            }
            variant_batch_size = MOD_NZ(cur_block_size, kPglBitTransposeBatch);
            variant_batch_size_rounded_up = variant_batch_size;
            const uint32_t variant_batch_size_rem = variant_batch_size % kBitsPerWord;
            if (variant_batch_size_rem) {
              const uint32_t trailing_variant_ct = kBitsPerWord - variant_batch_size_rem;
              variant_batch_size_rounded_up += trailing_variant_ct;
              fill_ulong_zero(trailing_variant_ct * row_end_idxaw, &(splitbuf_hom[variant_batch_size * row_end_idxaw]));
              fill_ulong_zero(trailing_variant_ct * row_end_idxaw, &(splitbuf_ref2het[variant_batch_size * row_end_idxaw]));
            }
          }
          uintptr_t* hom_iter = splitbuf_hom;
          uintptr_t* ref2het_iter = splitbuf_ref2het;
          for (uint32_t uii = 0; uii < variant_batch_size; ++uii, ++variant_uidx) {
            next_set_unsafe_ck(variant_include, &variant_uidx);
            reterr = pgr_read_refalt1_genovec_subset_unsafe(cur_sample_include, sample_include_cumulative_popcounts, row_end_idx, variant_uidx, simple_pgrp, loadbuf);
            if (reterr) {
              goto calc_king_ret_PGR_FAIL;
            }
            set_trailing_quaters(row_end_idx, loadbuf);
            split_hom_ref2het_unsafew(loadbuf, row_end_idxaw2, hom_iter, ref2het_iter);
            hom_iter = &(hom_iter[row_end_idxaw]);
            ref2het_iter = &(ref2het_iter[row_end_idxaw]);
          }
          // uintptr_t* read_iter = loadbuf;
          uintptr_t* write_hom_iter = &(cur_smaj_hom[write_batch_idx * kPglBitTransposeWords]);
          uintptr_t* write_ref2het_iter = &(cur_smaj_ref2het[write_batch_idx * kPglBitTransposeWords]);
          uint32_t sample_batch_idx = 0;
          uint32_t write_batch_size = kPglBitTransposeBatch;
          while (1) {
            if (sample_batch_idx >= sample_batch_ct_m1) {
              if (sample_batch_idx > sample_batch_ct_m1) {
                break;
              }
              write_batch_size = MOD_NZ(row_end_idx, kPglBitTransposeBatch);
            }
            // bugfix: read_batch_size must be rounded up to word boundary,
            // since we want to one-out instead of zero-out the trailing bits
            //
            // bugfix: if we always use kPglBitTransposeBatch instead of
            // variant_batch_size_rounded_up, we read/write past the
            // kKingMultiplex limit and clobber the first variants of the next
            // sample with garbage.
            transpose_bitblock(&(splitbuf_hom[sample_batch_idx * kPglBitTransposeWords]), row_end_idxaw, kKingMultiplexWords, variant_batch_size_rounded_up, write_batch_size, write_hom_iter, vecaligned_buf);
            transpose_bitblock(&(splitbuf_ref2het[sample_batch_idx * kPglBitTransposeWords]), row_end_idxaw, kKingMultiplexWords, variant_batch_size_rounded_up, write_batch_size, write_ref2het_iter, vecaligned_buf);
            ++sample_batch_idx;
            write_hom_iter = &(write_hom_iter[kKingMultiplex * kPglBitTransposeWords]);
            write_ref2het_iter = &(write_ref2het_iter[kKingMultiplex * kPglBitTransposeWords]);
          }
          ++write_batch_idx;
        }
        const uint32_t cur_block_sizew = BITCT_TO_WORDCT(cur_block_size);
        if (cur_block_sizew < kKingMultiplexWords) {
          uintptr_t* write_hom_iter = &(cur_smaj_hom[cur_block_sizew]);
          uintptr_t* write_ref2het_iter = &(cur_smaj_ref2het[cur_block_sizew]);
          const uint32_t write_word_ct = kKingMultiplexWords - cur_block_sizew;
          for (uint32_t sample_idx = 0; sample_idx < row_end_idx; ++sample_idx) {
            fill_ulong_zero(write_word_ct, write_hom_iter);
            fill_ulong_zero(write_word_ct, write_ref2het_iter);
            write_hom_iter = &(write_hom_iter[kKingMultiplexWords]);
            write_ref2het_iter = &(write_ref2het_iter[kKingMultiplexWords]);
          }
        }
        if (variants_completed) {
          join_threads3z(&ts);
        } else {
          ts.thread_func_ptr = calc_king_thread;
        }
        // this update must occur after join_threads3z() call
        ts.is_last_block = (variants_completed + cur_block_size == variant_ct);
        if (spawn_threads3z(variants_completed, &ts)) {
          goto calc_king_ret_THREAD_CREATE_FAIL;
        }
        printf("\r%s pass %u/%u: %u variants complete.", flagname, pass_idx_p1, pass_ct, variants_completed);
        fflush(stdout);
        variants_completed += cur_block_size;
        parity = 1 - parity;
      } while (!ts.is_last_block);
      join_threads3z(&ts);
      if (matrix_shape || (king_modifier & kfKingColAll)) {
        printf("\r%s pass %u/%u: Writing...                   \b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b", flagname, pass_idx_p1, pass_ct);
        fflush(stdout);
        // allow simultaneous --make-king + --make-king-table
        if (matrix_shape) {
          if (!(king_modifier & (kfKingMatrixBin | kfKingMatrixBin4))) {
            const uint32_t is_squarex = king_modifier & (kfKingMatrixSq | kfKingMatrixSq0);
            const uint32_t is_square0 = king_modifier & kfKingMatrixSq0;
            uint32_t* results_iter = g_king_counts;
            uint32_t sample_idx1 = row_start_idx;
            if (is_squarex && (!parallel_idx) && (pass_idx_p1)) {
              // dump "empty" first row
              sample_idx1 = 0;
            }
            for (; sample_idx1 < row_end_idx; ++sample_idx1) {
              for (uint32_t sample_idx2 = 0; sample_idx2 < sample_idx1; ++sample_idx2) {
                const double kinship_coeff = compute_kinship(results_iter);
                if (kinship_table && (kinship_coeff > king_cutoff)) {
                  SET_BIT(sample_idx2, &(kinship_table[sample_idx1 * sample_ctl]));
                  SET_BIT(sample_idx1, &(kinship_table[sample_idx2 * sample_ctl]));
                }
                cswritep = dtoa_g(kinship_coeff, cswritep);
                *cswritep++ = '\t';
                results_iter = &(results_iter[homhom_needed_p4]);
              }
              if (is_squarex) {
                cswritep = memcpyl3a(cswritep, "0.5");
                if (is_square0) {
                  // (roughly same performance as creating a tab-zero constant
                  // buffer in advance)
                  const uint32_t zcount = sample_ct - sample_idx1 - 1;
                  const uint32_t wct = DIV_UP(zcount, kBytesPerWord / 2);
                  // assumes little-endian
                  const uintptr_t tabzero_word = 0x3009 * kMask0001;
#ifdef __arm__
#  error "Unaligned accesses in calc_king()."
#endif
                  uintptr_t* writep_alias = R_CAST(uintptr_t*, cswritep);
                  for (uintptr_t widx = 0; widx < wct; ++widx) {
                    *writep_alias++ = tabzero_word;
                  }
                  cswritep = &(cswritep[zcount * 2]);
                } else {
                  const uint32_t* results_iter2 = &(results_iter[sample_idx1 * homhom_needed_p4]);
                  // 0
                  // 1  2
                  // 3  4  5
                  // 6  7  8  9
                  // 10 11 12 13 14

                  // sample_idx1 = 0: [0] 0 1 3 6 10...
                  // sample_idx1 = 1: [1] 2 4 7 11...
                  // sample_idx1 = 2: [3] 5 8 12...
                  // sample_idx1 = 3: [6] 9 13...
                  for (uint32_t sample_idx2 = sample_idx1 + 1; sample_idx2 < sample_ct; ++sample_idx2) {
                    *cswritep++ = '\t';
                    cswritep = dtoa_g(compute_kinship(results_iter2), cswritep);
                    results_iter2 = &(results_iter2[sample_idx2 * homhom_needed_p4]);
                  }
                }
                ++cswritep;
              }
              decr_append_binary_eoln(&cswritep);
              if (cswrite(&css, &cswritep)) {
                goto calc_king_ret_WRITE_FAIL;
              }
            }
          } else {
            // binary matrix output
            // er, probably want to revise this so there's less duplicated code
            // from text matrix output...
            const uint32_t is_squarex = king_modifier & (kfKingMatrixSq | kfKingMatrixSq0);
            const uint32_t is_square0 = king_modifier & kfKingMatrixSq0;
            uint32_t* results_iter = g_king_counts;
            uint32_t sample_idx1 = row_start_idx;
            if (is_squarex && (!parallel_idx)) {
              sample_idx1 = 0;
            }
            if (king_modifier & kfKingMatrixBin4) {
              float* write_row = R_CAST(float*, numbuf);
              uintptr_t row_byte_ct = sample_ct * sizeof(float);
              for (; sample_idx1 < row_end_idx; ++sample_idx1) {
                for (uint32_t sample_idx2 = 0; sample_idx2 < sample_idx1; ++sample_idx2) {
                  const double kinship_coeff = compute_kinship(results_iter);
                  if (kinship_table && (kinship_coeff > king_cutoff)) {
                    SET_BIT(sample_idx2, &(kinship_table[sample_idx1 * sample_ctl]));
                    SET_BIT(sample_idx1, &(kinship_table[sample_idx2 * sample_ctl]));
                  }
                  write_row[sample_idx2] = S_CAST(float, kinship_coeff);
                  results_iter = &(results_iter[homhom_needed_p4]);
                }
                if (is_squarex) {
                  write_row[sample_idx1] = 0.5f;
                  if (is_square0) {
                    const uint32_t right_fill_idx = sample_idx1 + 1;
                    fill_float_zero(sample_ct - right_fill_idx, &(write_row[right_fill_idx]));
                  } else {
                    const uint32_t* results_iter2 = &(results_iter[sample_idx1 * homhom_needed_p4]);
                    for (uint32_t sample_idx2 = sample_idx1 + 1; sample_idx2 < sample_ct; ++sample_idx2) {
                      write_row[sample_idx2] = S_CAST(float, compute_kinship(results_iter2));
                      results_iter2 = &(results_iter2[sample_idx2 * homhom_needed_p4]);
                    }
                  }
                } else {
                  row_byte_ct = sample_idx1 * sizeof(float);
                }
                if (fwrite_checked(write_row, row_byte_ct, outfile)) {
                  goto calc_king_ret_WRITE_FAIL;
                }
              }
            } else {
              double* write_row = R_CAST(double*, numbuf);
              uintptr_t row_byte_ct = sample_ct * sizeof(double);
              for (; sample_idx1 < row_end_idx; ++sample_idx1) {
                for (uint32_t sample_idx2 = 0; sample_idx2 < sample_idx1; ++sample_idx2) {
                  const double kinship_coeff = compute_kinship(results_iter);
                  if (kinship_table && (kinship_coeff > king_cutoff)) {
                    SET_BIT(sample_idx2, &(kinship_table[sample_idx1 * sample_ctl]));
                    SET_BIT(sample_idx1, &(kinship_table[sample_idx2 * sample_ctl]));
                  }
                  write_row[sample_idx2] = kinship_coeff;
                  results_iter = &(results_iter[homhom_needed_p4]);
                }
                if (is_squarex) {
                  write_row[sample_idx1] = 0.5;
                  if (is_square0) {
                    const uint32_t right_fill_idx = sample_idx1 + 1;
                    fill_double_zero(sample_ct - right_fill_idx, &(write_row[right_fill_idx]));
                  } else {
                    const uint32_t* results_iter2 = &(results_iter[sample_idx1 * homhom_needed_p4]);
                    for (uint32_t sample_idx2 = sample_idx1 + 1; sample_idx2 < sample_ct; ++sample_idx2) {
                      write_row[sample_idx2] = compute_kinship(results_iter2);
                      results_iter2 = &(results_iter2[sample_idx2 * homhom_needed_p4]);
                    }
                  }
                } else {
                  row_byte_ct = sample_idx1 * sizeof(double);
                }
                if (fwrite_checked(write_row, row_byte_ct, outfile)) {
                  goto calc_king_ret_WRITE_FAIL;
                }
              }
            }
          }
        }
        if (king_modifier & kfKingColAll) {
          uintptr_t* kinship_table_backup = nullptr;
          if (matrix_shape) {
            // We already updated the table; don't do it again.
            kinship_table_backup = kinship_table;
            kinship_table = nullptr;
          }
          const uint32_t king_col_id = king_modifier & kfKingColId;
          const uint32_t king_col_nsnp = king_modifier & kfKingColNsnp;
          const uint32_t king_col_hethet = king_modifier & kfKingColHethet;
          const uint32_t king_col_ibs0 = king_modifier & kfKingColIbs0;
          const uint32_t king_col_ibs1 = king_modifier & kfKingColIbs1;
          const uint32_t king_col_kinship = king_modifier & kfKingColKinship;
          const uint32_t report_counts = king_modifier & kfKingCounts;
          uint32_t* results_iter = g_king_counts;
          double nonmiss_recip = 0.0;
          for (uint32_t sample_idx1 = row_start_idx; sample_idx1 < row_end_idx; ++sample_idx1) {
            const char* sample_augid1 = &(collapsed_sample_augids[max_sample_augid_blen * sample_idx1]);
            uint32_t sample_augid1_len = strlen(sample_augid1);
            for (uint32_t sample_idx2 = 0; sample_idx2 < sample_idx1; ++sample_idx2, results_iter = &(results_iter[homhom_needed_p4])) {
              const uint32_t ibs0_ct = results_iter[0];
              const uint32_t hethet_ct = results_iter[1];
              const uint32_t het2hom1_ct = results_iter[2];
              const uint32_t het1hom2_ct = results_iter[3];
              const intptr_t smaller_het_ct = hethet_ct + MINV(het1hom2_ct, het2hom1_ct);
              const double kinship_coeff = 0.5 - (S_CAST(double, 4 * S_CAST(intptr_t, ibs0_ct) + het1hom2_ct + het2hom1_ct) / S_CAST(double, 4 * smaller_het_ct));
              if (kinship_table && (kinship_coeff > king_cutoff)) {
                SET_BIT(sample_idx2, &(kinship_table[sample_idx1 * sample_ctl]));
                SET_BIT(sample_idx1, &(kinship_table[sample_idx2 * sample_ctl]));
              }
              // edge case fix (18 Nov 2017): kinship_coeff can be -inf when
              // smaller_het_ct is zero.  Don't filter those lines out when
              // --king-table-filter wasn't specified.
              if ((king_table_filter != -DBL_MAX) && (kinship_coeff < king_table_filter)) {
                ++king_table_filter_ct;
                continue;
              }
              if (king_col_id) {
                cswritetp = memcpyax(cswritetp, sample_augid1, sample_augid1_len, '\t');
                cswritetp = strcpyax(cswritetp, &(collapsed_sample_augids[max_sample_augid_blen * sample_idx2]), '\t');
              }
              if (homhom_needed_p4 == 5) {
                const uint32_t homhom_ct = results_iter[4];
                const uint32_t nonmiss_ct = het1hom2_ct + het2hom1_ct + homhom_ct + hethet_ct;
                if (king_col_nsnp) {
                  cswritetp = uint32toa_x(nonmiss_ct, '\t', cswritetp);
                }
                if (!report_counts) {
                  nonmiss_recip = 1.0 / u31tod(nonmiss_ct);
                }
              }
              if (king_col_hethet) {
                if (report_counts) {
                  cswritetp = uint32toa(hethet_ct, cswritetp);
                } else {
                  cswritetp = dtoa_g(nonmiss_recip * u31tod(hethet_ct), cswritetp);
                }
                *cswritetp++ = '\t';
              }
              if (king_col_ibs0) {
                if (report_counts) {
                  cswritetp = uint32toa(ibs0_ct, cswritetp);
                } else {
                  cswritetp = dtoa_g(nonmiss_recip * u31tod(ibs0_ct), cswritetp);
                }
                *cswritetp++ = '\t';
              }
              if (king_col_ibs1) {
                if (report_counts) {
                  cswritetp = uint32toa_x(het1hom2_ct, '\t', cswritetp);
                  cswritetp = uint32toa(het2hom1_ct, cswritetp);
                } else {
                  cswritetp = dtoa_g(nonmiss_recip * u31tod(het1hom2_ct), cswritetp);
                  *cswritetp++ = '\t';
                  cswritetp = dtoa_g(nonmiss_recip * u31tod(het2hom1_ct), cswritetp);
                }
                *cswritetp++ = '\t';
              }
              if (king_col_kinship) {
                cswritetp = dtoa_g(kinship_coeff, cswritetp);
                ++cswritetp;
              }
              decr_append_binary_eoln(&cswritetp);
              if (cswrite(&csst, &cswritetp)) {
                goto calc_king_ret_WRITE_FAIL;
              }
            }
          }

          if (matrix_shape) {
            kinship_table = kinship_table_backup;
          }
        }
      } else {
        printf("\r%s pass %u/%u: Condensing...                \b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b", flagname, pass_idx_p1, pass_ct);
        fflush(stdout);
        uint32_t* results_iter = g_king_counts;
        for (uint32_t sample_idx1 = row_start_idx; sample_idx1 < row_end_idx; ++sample_idx1) {
          for (uint32_t sample_idx2 = 0; sample_idx2 < sample_idx1; ++sample_idx2) {
            const double kinship_coeff = compute_kinship(results_iter);
            if (kinship_coeff > king_cutoff) {
              SET_BIT(sample_idx2, &(kinship_table[sample_idx1 * sample_ctl]));
              SET_BIT(sample_idx1, &(kinship_table[sample_idx2 * sample_ctl]));
            }
            results_iter = &(results_iter[homhom_needed_p4]);
          }
        }
      }
    }
    putc_unlocked('\n', stdout);
    LOGPRINTF("%s: %u variant%s processed.\n", flagname, variant_ct, (variant_ct == 1)? "" : "s");
    // end-of-loop operations
    const uint32_t no_idheader = (king_modifier / kfKingNoIdheader) & 1;
    if (matrix_shape) {
      if (!(king_modifier & (kfKingMatrixBin | kfKingMatrixBin4))) {
        if (cswrite_close_null(&css, cswritep)) {
          goto calc_king_ret_WRITE_FAIL;
        }
      } else {
        if (fclose_null(&outfile)) {
          goto calc_king_ret_WRITE_FAIL;
        }
      }
      // Necessary to regenerate filename since it may have been overwritten by
      // --make-king-table.
      set_king_matrix_fname(king_modifier, parallel_idx, parallel_tot, outname_end);

      char* write_iter = strcpya(g_logbuf, "Results written to ");
      write_iter = strcpya(write_iter, outname);
      write_iter = strcpya(write_iter, " and ");
      snprintf(&(outname_end[5]), kMaxOutfnameExtBlen - 5, ".id");
      write_iter = strcpya(write_iter, outname);
      snprintf(write_iter, kLogbufSize - 2 * kPglFnamesize - 64, " .\n");
      wordwrapb(0);
      logprintb();
      reterr = write_sample_ids(sample_include, sample_ids, sids, outname, sample_ct, max_sample_id_blen, max_sid_blen, no_idheader);
      if (reterr) {
        goto calc_king_ret_1;
      }
    }
    if (king_modifier & kfKingColAll) {
      if (cswrite_close_null(&csst, cswritetp)) {
        goto calc_king_ret_WRITE_FAIL;
      }
      set_king_table_fname(king_modifier, parallel_idx, parallel_tot, outname_end);
      char* write_iter = strcpya(g_logbuf, "Results written to ");
      write_iter = strcpya(write_iter, outname);
      if ((!parallel_idx) && (!(king_modifier & kfKingColId))) {
        write_iter = strcpya(write_iter, " and ");
        strcpya(&(outname_end[5]), ".id");
        write_iter = strcpya(write_iter, outname);
        snprintf(write_iter, kLogbufSize - 2 * kPglFnamesize - 64, " .\n");
        wordwrapb(0);
        logprintb();
        reterr = write_sample_ids(sample_include, sample_ids, sids, outname, sample_ct, max_sample_id_blen, max_sid_blen, no_idheader);
        if (reterr) {
          goto calc_king_ret_1;
        }
      } else {
        snprintf(write_iter, kLogbufSize - kPglFnamesize - 64, " .\n");
        wordwrapb(0);
        logprintb();
      }
      if (king_table_filter != -DBL_MAX) {
        const uint64_t grand_tot_cells = (S_CAST(uint64_t, grand_row_end_idx) * (grand_row_end_idx - 1) - S_CAST(uint64_t, grand_row_start_idx) * (grand_row_start_idx - 1)) / 2;
        const uint64_t reported_ct = grand_tot_cells - king_table_filter_ct;
        LOGPRINTF("--king-table-filter: %" PRIu64 " relationship%s reported (%" PRIu64 " filtered out).\n", reported_ct, (reported_ct == 1)? "" : "s", king_table_filter_ct);
      }
    }
    if (kinship_table) {
      bigstack_reset(sample_include_cumulative_popcounts);
      *sample_ct_ptr = sample_ct;
      if (kinship_prune_destructive(kinship_table, sample_include, sample_ct_ptr)) {
        goto calc_king_ret_NOMEM;
      }
    }
  }
  while (0) {
  calc_king_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  calc_king_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  calc_king_ret_PGR_FAIL:
    if (reterr != kPglRetReadFail) {
      logerrprint("Error: Malformed .pgen file.\n");
    }
    break;
  calc_king_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  calc_king_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  calc_king_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 calc_king_ret_1:
  threads3z_cleanup(&ts, nullptr);
  cswrite_close_cond(&csst, cswritetp);
  cswrite_close_cond(&css, cswritep);
  fclose_cond(outfile);
  bigstack_reset(bigstack_mark);
  return reterr;
}

#ifdef USE_SSE42
void incr_king_subset(const uint32_t* loaded_sample_idx_pairs, const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts) {
  const uint32_t* sample_idx_pair_iter = &(loaded_sample_idx_pairs[(2 * k1LU) * start_idx]);
  const uint32_t* sample_idx_pair_stop = &(loaded_sample_idx_pairs[(2 * k1LU) * end_idx]);
  uint32_t* king_counts_iter = &(king_counts[(4 * k1LU) * start_idx]);
  while (sample_idx_pair_iter != sample_idx_pair_stop) {
    // technically overflows for huge sample_ct
    const uint32_t first_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uint32_t second_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uintptr_t* first_hom = &(smaj_hom[first_offset]);
    const uintptr_t* first_ref2het = &(smaj_ref2het[first_offset]);
    const uintptr_t* second_hom = &(smaj_hom[second_offset]);
    const uintptr_t* second_ref2het = &(smaj_ref2het[second_offset]);
    uint32_t acc_ibs0 = 0;
    uint32_t acc_hethet = 0;
    uint32_t acc_het2hom1 = 0;
    uint32_t acc_het1hom2 = 0;
    for (uint32_t widx = 0; widx < kKingMultiplexWords; ++widx) {
      const uintptr_t hom1 = first_hom[widx];
      const uintptr_t hom2 = second_hom[widx];
      const uintptr_t ref2het1 = first_ref2het[widx];
      const uintptr_t ref2het2 = second_ref2het[widx];
      const uintptr_t homhom = hom1 & hom2;
      const uintptr_t het1 = ref2het1 & (~hom1);
      const uintptr_t het2 = ref2het2 & (~hom2);
      acc_ibs0 += popcount_long((ref2het1 ^ ref2het2) & homhom);
      acc_hethet += popcount_long(het1 & het2);
      acc_het2hom1 += popcount_long(hom1 & het2);
      acc_het1hom2 += popcount_long(hom2 & het1);
    }
    *king_counts_iter++ += acc_ibs0;
    *king_counts_iter++ += acc_hethet;
    *king_counts_iter++ += acc_het2hom1;
    *king_counts_iter++ += acc_het1hom2;
  }
}

void incr_king_subset_homhom(const uint32_t* loaded_sample_idx_pairs, const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts) {
  const uint32_t* sample_idx_pair_iter = &(loaded_sample_idx_pairs[(2 * k1LU) * start_idx]);
  const uint32_t* sample_idx_pair_stop = &(loaded_sample_idx_pairs[(2 * k1LU) * end_idx]);
  uint32_t* king_counts_iter = &(king_counts[(5 * k1LU) * start_idx]);
  while (sample_idx_pair_iter != sample_idx_pair_stop) {
    // technically overflows for huge sample_ct
    const uint32_t first_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uint32_t second_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uintptr_t* first_hom = &(smaj_hom[first_offset]);
    const uintptr_t* first_ref2het = &(smaj_ref2het[first_offset]);
    const uintptr_t* second_hom = &(smaj_hom[second_offset]);
    const uintptr_t* second_ref2het = &(smaj_ref2het[second_offset]);
    uint32_t acc_homhom = 0;
    uint32_t acc_ibs0 = 0;
    uint32_t acc_hethet = 0;
    uint32_t acc_het2hom1 = 0;
    uint32_t acc_het1hom2 = 0;
    for (uint32_t widx = 0; widx < kKingMultiplexWords; ++widx) {
      const uintptr_t hom1 = first_hom[widx];
      const uintptr_t hom2 = second_hom[widx];
      const uintptr_t ref2het1 = first_ref2het[widx];
      const uintptr_t ref2het2 = second_ref2het[widx];
      const uintptr_t homhom = hom1 & hom2;
      const uintptr_t het1 = ref2het1 & (~hom1);
      const uintptr_t het2 = ref2het2 & (~hom2);
      acc_homhom += popcount_long(homhom);
      acc_ibs0 += popcount_long((ref2het1 ^ ref2het2) & homhom);
      acc_hethet += popcount_long(het1 & het2);
      acc_het2hom1 += popcount_long(hom1 & het2);
      acc_het1hom2 += popcount_long(hom2 & het1);
    }
    *king_counts_iter++ += acc_ibs0;
    *king_counts_iter++ += acc_hethet;
    *king_counts_iter++ += acc_het2hom1;
    *king_counts_iter++ += acc_het1hom2;
    *king_counts_iter++ += acc_homhom;
  }
}
#else
void incr_king_subset(const uint32_t* loaded_sample_idx_pairs, const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts) {
  const vul_t m1 = VCONST_UL(kMask5555);
  const vul_t m2 = VCONST_UL(kMask3333);
  const vul_t m4 = VCONST_UL(kMask0F0F);
  const uint32_t* sample_idx_pair_iter = &(loaded_sample_idx_pairs[(2 * k1LU) * start_idx]);
  const uint32_t* sample_idx_pair_stop = &(loaded_sample_idx_pairs[(2 * k1LU) * end_idx]);
  uint32_t* king_counts_iter = &(king_counts[(4 * k1LU) * start_idx]);
  while (sample_idx_pair_iter != sample_idx_pair_stop) {
    // technically overflows for huge sample_ct
    const uint32_t first_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uint32_t second_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const vul_t* first_hom = R_CAST(const vul_t*, &(smaj_hom[first_offset]));
    const vul_t* first_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[first_offset]));
    const vul_t* second_hom = R_CAST(const vul_t*, &(smaj_hom[second_offset]));
    const vul_t* second_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[second_offset]));
    univec_t acc_ibs0;
    univec_t acc_hethet;
    univec_t acc_het2hom1;
    univec_t acc_het1hom2;
    acc_ibs0.vi = vul_setzero();
    acc_hethet.vi = vul_setzero();
    acc_het2hom1.vi = vul_setzero();
    acc_het1hom2.vi = vul_setzero();
    for (uint32_t vec_idx = 0; vec_idx < kKingMultiplexVecs; vec_idx += 3) {
      vul_t hom1 = first_hom[vec_idx];
      vul_t hom2 = second_hom[vec_idx];
      vul_t ref2het1 = first_ref2het[vec_idx];
      vul_t ref2het2 = second_ref2het[vec_idx];
      vul_t het1 = ref2het1 & (~hom1);
      vul_t het2 = ref2het2 & (~hom2);
      vul_t agg_ibs0 = (ref2het1 ^ ref2het2) & (hom1 & hom2);
      vul_t agg_hethet = het1 & het2;
      vul_t agg_het2hom1 = hom1 & het2;
      vul_t agg_het1hom2 = hom2 & het1;
      agg_ibs0 = agg_ibs0 - (vul_rshift(agg_ibs0, 1) & m1);
      agg_hethet = agg_hethet - (vul_rshift(agg_hethet, 1) & m1);
      agg_het2hom1 = agg_het2hom1 - (vul_rshift(agg_het2hom1, 1) & m1);
      agg_het1hom2 = agg_het1hom2 - (vul_rshift(agg_het1hom2, 1) & m1);
      agg_ibs0 = (agg_ibs0 & m2) + (vul_rshift(agg_ibs0, 2) & m2);
      agg_hethet = (agg_hethet & m2) + (vul_rshift(agg_hethet, 2) & m2);
      agg_het2hom1 = (agg_het2hom1 & m2) + (vul_rshift(agg_het2hom1, 2) & m2);
      agg_het1hom2 = (agg_het1hom2 & m2) + (vul_rshift(agg_het1hom2, 2) & m2);

      for (uint32_t offset = 1; offset < 3; ++offset) {
        hom1 = first_hom[vec_idx + offset];
        hom2 = second_hom[vec_idx + offset];
        ref2het1 = first_ref2het[vec_idx + offset];
        ref2het2 = second_ref2het[vec_idx + offset];
        het1 = ref2het1 & (~hom1);
        het2 = ref2het2 & (~hom2);
        vul_t cur_ibs0 = (ref2het1 ^ ref2het2) & (hom1 & hom2);
        vul_t cur_hethet = het1 & het2;
        vul_t cur_het2hom1 = hom1 & het2;
        vul_t cur_het1hom2 = hom2 & het1;
        cur_ibs0 = cur_ibs0 - (vul_rshift(cur_ibs0, 1) & m1);
        cur_hethet = cur_hethet - (vul_rshift(cur_hethet, 1) & m1);
        cur_het2hom1 = cur_het2hom1 - (vul_rshift(cur_het2hom1, 1) & m1);
        cur_het1hom2 = cur_het1hom2 - (vul_rshift(cur_het1hom2, 1) & m1);
        agg_ibs0 += (cur_ibs0 & m2) + (vul_rshift(cur_ibs0, 2) & m2);
        agg_hethet += (cur_hethet & m2) + (vul_rshift(cur_hethet, 2) & m2);
        agg_het2hom1 += (cur_het2hom1 & m2) + (vul_rshift(cur_het2hom1, 2) & m2);
        agg_het1hom2 += (cur_het1hom2 & m2) + (vul_rshift(cur_het1hom2, 2) & m2);
      }
      acc_ibs0.vi = acc_ibs0.vi + (agg_ibs0 & m4) + (vul_rshift(agg_ibs0, 4) & m4);
      acc_hethet.vi = acc_hethet.vi + (agg_hethet & m4) + (vul_rshift(agg_hethet, 4) & m4);
      acc_het2hom1.vi = acc_het2hom1.vi + (agg_het2hom1 & m4) + (vul_rshift(agg_het2hom1, 4) & m4);
      acc_het1hom2.vi = acc_het1hom2.vi + (agg_het1hom2 & m4) + (vul_rshift(agg_het1hom2, 4) & m4);
    }
    const vul_t m8 = VCONST_UL(kMask00FF);
    acc_ibs0.vi = (acc_ibs0.vi & m8) + (vul_rshift(acc_ibs0.vi, 8) & m8);
    acc_hethet.vi = (acc_hethet.vi & m8) + (vul_rshift(acc_hethet.vi, 8) & m8);
    acc_het2hom1.vi = (acc_het2hom1.vi & m8) + (vul_rshift(acc_het2hom1.vi, 8) & m8);
    acc_het1hom2.vi = (acc_het1hom2.vi & m8) + (vul_rshift(acc_het1hom2.vi, 8) & m8);
    *king_counts_iter++ += univec_hsum_16bit(acc_ibs0);
    *king_counts_iter++ += univec_hsum_16bit(acc_hethet);
    *king_counts_iter++ += univec_hsum_16bit(acc_het2hom1);
    *king_counts_iter++ += univec_hsum_16bit(acc_het1hom2);
  }
}

void incr_king_subset_homhom(const uint32_t* loaded_sample_idx_pairs, const uintptr_t* smaj_hom, const uintptr_t* smaj_ref2het, uint32_t start_idx, uint32_t end_idx, uint32_t* king_counts) {
  const vul_t m1 = VCONST_UL(kMask5555);
  const vul_t m2 = VCONST_UL(kMask3333);
  const vul_t m4 = VCONST_UL(kMask0F0F);
  const uint32_t* sample_idx_pair_iter = &(loaded_sample_idx_pairs[(2 * k1LU) * start_idx]);
  const uint32_t* sample_idx_pair_stop = &(loaded_sample_idx_pairs[(2 * k1LU) * end_idx]);
  uint32_t* king_counts_iter = &(king_counts[(5 * k1LU) * start_idx]);
  while (sample_idx_pair_iter != sample_idx_pair_stop) {
    // technically overflows for huge sample_ct
    const uint32_t first_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const uint32_t second_offset = (*sample_idx_pair_iter++) * kKingMultiplexWords;
    const vul_t* first_hom = R_CAST(const vul_t*, &(smaj_hom[first_offset]));
    const vul_t* first_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[first_offset]));
    const vul_t* second_hom = R_CAST(const vul_t*, &(smaj_hom[second_offset]));
    const vul_t* second_ref2het = R_CAST(const vul_t*, &(smaj_ref2het[second_offset]));
    univec_t acc_homhom;
    univec_t acc_ibs0;
    univec_t acc_hethet;
    univec_t acc_het2hom1;
    univec_t acc_het1hom2;
    acc_homhom.vi = vul_setzero();
    acc_ibs0.vi = vul_setzero();
    acc_hethet.vi = vul_setzero();
    acc_het2hom1.vi = vul_setzero();
    acc_het1hom2.vi = vul_setzero();
    for (uint32_t vec_idx = 0; vec_idx < kKingMultiplexVecs; vec_idx += 3) {
      vul_t hom1 = first_hom[vec_idx];
      vul_t hom2 = second_hom[vec_idx];
      vul_t ref2het1 = first_ref2het[vec_idx];
      vul_t ref2het2 = second_ref2het[vec_idx];
      vul_t agg_homhom = hom1 & hom2;
      vul_t het1 = ref2het1 & (~hom1);
      vul_t het2 = ref2het2 & (~hom2);
      vul_t agg_ibs0 = (ref2het1 ^ ref2het2) & agg_homhom;
      vul_t agg_hethet = het1 & het2;
      vul_t agg_het2hom1 = hom1 & het2;
      vul_t agg_het1hom2 = hom2 & het1;
      agg_homhom = agg_homhom - (vul_rshift(agg_homhom, 1) & m1);
      agg_ibs0 = agg_ibs0 - (vul_rshift(agg_ibs0, 1) & m1);
      agg_hethet = agg_hethet - (vul_rshift(agg_hethet, 1) & m1);
      agg_het2hom1 = agg_het2hom1 - (vul_rshift(agg_het2hom1, 1) & m1);
      agg_het1hom2 = agg_het1hom2 - (vul_rshift(agg_het1hom2, 1) & m1);
      agg_homhom = (agg_homhom & m2) + (vul_rshift(agg_homhom, 2) & m2);
      agg_ibs0 = (agg_ibs0 & m2) + (vul_rshift(agg_ibs0, 2) & m2);
      agg_hethet = (agg_hethet & m2) + (vul_rshift(agg_hethet, 2) & m2);
      agg_het2hom1 = (agg_het2hom1 & m2) + (vul_rshift(agg_het2hom1, 2) & m2);
      agg_het1hom2 = (agg_het1hom2 & m2) + (vul_rshift(agg_het1hom2, 2) & m2);

      for (uint32_t offset = 1; offset < 3; ++offset) {
        hom1 = first_hom[vec_idx + offset];
        hom2 = second_hom[vec_idx + offset];
        ref2het1 = first_ref2het[vec_idx + offset];
        ref2het2 = second_ref2het[vec_idx + offset];
        vul_t cur_homhom = hom1 & hom2;
        het1 = ref2het1 & (~hom1);
        het2 = ref2het2 & (~hom2);
        vul_t cur_ibs0 = (ref2het1 ^ ref2het2) & cur_homhom;
        vul_t cur_hethet = het1 & het2;
        vul_t cur_het2hom1 = hom1 & het2;
        vul_t cur_het1hom2 = hom2 & het1;
        cur_homhom = cur_homhom - (vul_rshift(cur_homhom, 1) & m1);
        cur_ibs0 = cur_ibs0 - (vul_rshift(cur_ibs0, 1) & m1);
        cur_hethet = cur_hethet - (vul_rshift(cur_hethet, 1) & m1);
        cur_het2hom1 = cur_het2hom1 - (vul_rshift(cur_het2hom1, 1) & m1);
        cur_het1hom2 = cur_het1hom2 - (vul_rshift(cur_het1hom2, 1) & m1);
        agg_homhom += (cur_homhom & m2) + (vul_rshift(cur_homhom, 2) & m2);
        agg_ibs0 += (cur_ibs0 & m2) + (vul_rshift(cur_ibs0, 2) & m2);
        agg_hethet += (cur_hethet & m2) + (vul_rshift(cur_hethet, 2) & m2);
        agg_het2hom1 += (cur_het2hom1 & m2) + (vul_rshift(cur_het2hom1, 2) & m2);
        agg_het1hom2 += (cur_het1hom2 & m2) + (vul_rshift(cur_het1hom2, 2) & m2);
      }
      acc_homhom.vi = acc_homhom.vi + (agg_homhom & m4) + (vul_rshift(agg_homhom, 4) & m4);
      acc_ibs0.vi = acc_ibs0.vi + (agg_ibs0 & m4) + (vul_rshift(agg_ibs0, 4) & m4);
      acc_hethet.vi = acc_hethet.vi + (agg_hethet & m4) + (vul_rshift(agg_hethet, 4) & m4);
      acc_het2hom1.vi = acc_het2hom1.vi + (agg_het2hom1 & m4) + (vul_rshift(agg_het2hom1, 4) & m4);
      acc_het1hom2.vi = acc_het1hom2.vi + (agg_het1hom2 & m4) + (vul_rshift(agg_het1hom2, 4) & m4);
    }
    const vul_t m8 = VCONST_UL(kMask00FF);
    acc_homhom.vi = (acc_homhom.vi & m8) + (vul_rshift(acc_homhom.vi, 8) & m8);
    acc_ibs0.vi = (acc_ibs0.vi & m8) + (vul_rshift(acc_ibs0.vi, 8) & m8);
    acc_hethet.vi = (acc_hethet.vi & m8) + (vul_rshift(acc_hethet.vi, 8) & m8);
    acc_het2hom1.vi = (acc_het2hom1.vi & m8) + (vul_rshift(acc_het2hom1.vi, 8) & m8);
    acc_het1hom2.vi = (acc_het1hom2.vi & m8) + (vul_rshift(acc_het1hom2.vi, 8) & m8);
    *king_counts_iter++ += univec_hsum_16bit(acc_ibs0);
    *king_counts_iter++ += univec_hsum_16bit(acc_hethet);
    *king_counts_iter++ += univec_hsum_16bit(acc_het2hom1);
    *king_counts_iter++ += univec_hsum_16bit(acc_het1hom2);
    *king_counts_iter++ += univec_hsum_16bit(acc_homhom);
  }
}
#endif

THREAD_FUNC_DECL calc_king_table_subset_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t start_idx = g_thread_start[tidx];
  const uint32_t end_idx = g_thread_start[tidx + 1];
  const uint32_t homhom_needed = g_homhom_needed;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    if (homhom_needed) {
      incr_king_subset_homhom(g_loaded_sample_idx_pairs, g_smaj_hom[parity], g_smaj_ref2het[parity], start_idx, end_idx, g_king_counts);
    } else {
      incr_king_subset(g_loaded_sample_idx_pairs, g_smaj_hom[parity], g_smaj_ref2het[parity], start_idx, end_idx, g_king_counts);
    }
    if (is_last_block) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

pglerr_t king_table_subset_load(const char* sorted_xidbox, const uint32_t* xid_map, uintptr_t max_xid_blen, uintptr_t orig_sample_ct, double king_table_subset_thresh, xid_mode_t xid_mode, uint32_t id2_skip, uint32_t kinship_skip, uint32_t is_first_parallel_scan, uint64_t pair_idx_start, uint64_t pair_idx_stop, gzFile* gz_infilep, uint64_t* pair_idx_ptr, uint32_t* loaded_sample_idx_pairs, char* textbuf, char* idbuf) {
  uintptr_t line_idx = 1;
  pglerr_t reterr = kPglRetSuccess;
  {
    uint64_t pair_idx = *pair_idx_ptr;
    // Assumes header line already read if pair_idx == 0, and if pair_idx is
    // positive, we're that far into the file.
    // Assumes textbuf[kMaxMediumLine - 1] initialized to ' '.
    uint32_t* loaded_sample_idx_pairs_iter = loaded_sample_idx_pairs;
    while (1) {
      if (!gzgets(*gz_infilep, textbuf, kMaxMediumLine)) {
        if (!gzeof(*gz_infilep)) {
          goto king_table_subset_load_ret_READ_FAIL;
        }
        break;
      }
      ++line_idx;
      if (!textbuf[kMaxMediumLine - 1]) {
        goto king_table_subset_load_ret_LONG_LINE;
      }
      const char* textbuf_iter = skip_initial_spaces(textbuf);
      if (is_eoln_kns(*textbuf_iter)) {
        continue;
      }
      uint32_t sample_uidx1;
      if (sorted_xidbox_read_find(sorted_xidbox, xid_map, max_xid_blen, orig_sample_ct, 0, xid_mode, &textbuf_iter, &sample_uidx1, idbuf)) {
        if (!textbuf_iter) {
          goto king_table_subset_load_ret_MISSING_TOKENS;
        }
        continue;
      }
      textbuf_iter = skip_initial_spaces(textbuf_iter);
      if (id2_skip) {
        if (is_eoln_kns(*textbuf_iter)) {
          goto king_table_subset_load_ret_MISSING_TOKENS;
        }
        textbuf_iter = skip_initial_spaces(token_endnn(textbuf_iter));
      }
      uint32_t sample_uidx2;
      if (sorted_xidbox_read_find(sorted_xidbox, xid_map, max_xid_blen, orig_sample_ct, 0, xid_mode, &textbuf_iter, &sample_uidx2, idbuf)) {
        if (!textbuf_iter) {
          goto king_table_subset_load_ret_MISSING_TOKENS;
        }
        continue;
      }
      if (sample_uidx1 == sample_uidx2) {
        // could technically be due to unloaded SID, so use inconsistent-input
        // error code
        snprintf(g_logbuf, kLogbufSize, "Error: Identical sample IDs on line %" PRIuPTR " of --king-table-subset file.\n", line_idx);
        goto king_table_subset_load_ret_INCONSISTENT_INPUT_WW;
      }
      if (king_table_subset_thresh != -DBL_MAX) {
        textbuf_iter = skip_initial_spaces(textbuf_iter);
        textbuf_iter = next_token_multz(textbuf_iter, kinship_skip);
        if (!textbuf_iter) {
          goto king_table_subset_load_ret_MISSING_TOKENS;
        }
        double cur_kinship;
        if ((!scanadv_double(textbuf_iter, &cur_kinship)) || (cur_kinship < king_table_subset_thresh)) {
          continue;
        }
      }
      if (pair_idx < pair_idx_start) {
        ++pair_idx;
        continue;
      }
      *loaded_sample_idx_pairs_iter++ = sample_uidx1;
      *loaded_sample_idx_pairs_iter++ = sample_uidx2;
      ++pair_idx;
      if (pair_idx == pair_idx_stop) {
        if (!is_first_parallel_scan) {
          break;
        }
        // large --parallel job, first pass: count number of valid pairs, don't
        // save the remainder
        pair_idx_start = ~0LLU;
      }
    }
    *pair_idx_ptr = pair_idx;
  }
  while (0) {
  king_table_subset_load_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  king_table_subset_load_ret_LONG_LINE:
    snprintf(g_logbuf, kLogbufSize, "Error: Line %" PRIuPTR " of --king-table-subset file is pathologically long.\n", line_idx);
    wordwrapb(0);
    logerrprintb();
    reterr = kPglRetMalformedInput;
    break;
  king_table_subset_load_ret_MISSING_TOKENS:
    snprintf(g_logbuf, kLogbufSize, "Error: Line %" PRIuPTR " of --king-table-subset file has fewer tokens than expected.\n", line_idx);
  king_table_subset_load_ret_INCONSISTENT_INPUT_WW:
    wordwrapb(0);
    logerrprintb();
    reterr = kPglRetInconsistentInput;
    break;
  }
  return reterr;
}

pglerr_t calc_king_table_subset(const uintptr_t* orig_sample_include, const char* sample_ids, const char* sids, uintptr_t* variant_include, const chr_info_t* cip, const char* subset_fname, uint32_t raw_sample_ct, uint32_t orig_sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, uint32_t raw_variant_ct, uint32_t variant_ct, double king_table_filter, double king_table_subset_thresh, king_flags_t king_modifier, uint32_t parallel_idx, uint32_t parallel_tot, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  char* cswritep = nullptr;
  gzFile gz_infile = nullptr;
  compress_stream_state_t css;
  threads_state_t ts;
  pglerr_t reterr = kPglRetSuccess;
  cswrite_init_null(&css);
  init_threads3z(&ts);
  {
    if (is_set(cip->haploid_mask, 0)) {
      logerrprint("Error: --make-king-table cannot be used on haploid genomes.\n");
      goto calc_king_table_subset_ret_INCONSISTENT_INPUT;
    }
    reterr = conditional_allocate_non_autosomal_variants(cip, "--make-king-table", raw_variant_ct, &variant_include, &variant_ct);
    if (reterr) {
      goto calc_king_table_subset_ret_1;
    }
    // 1. Write output header line if necessary.
    // 2. Count number of relevant sample pairs (higher uidx in high 32 bits),
    //    and load as much as may be useful during first pass (usually there
    //    will be only one pass).
    // 3. If list is empty, error out.
    // 4. If --parallel, discard part of the list, then exit if remainder
    //    empty.
    // 5. If remainder of list is too large to process in one pass, determine
    //    number of necessary passes.  If output filename refers to the same
    //    thing as input file, append ~ to input filename.
    // Loop:
    // * Determine which sample indexes appear in this part of the list.
    //   Compute current cumulative_popcounts, perform uidx -> idx conversion.
    //   (Don't bother sorting the pairs, since that prevents
    //   --parallel/multipass mode from delivering the same results.)
    // * Execute usual KING-robust computation, write .kin0 entries.
    // * If not last pass, reload input .kin0, etc.
    //
    // Could store the pairs in a more compact manner, but can live with 50%
    // space bloat for now.
    const uint32_t raw_sample_ctl = BITCT_TO_WORDCT(raw_sample_ct);
    uint32_t sample_ctaw = BITCT_TO_ALIGNED_WORDCT(orig_sample_ct);
    uint32_t sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(orig_sample_ct);
    uint32_t king_bufsizew = kKingMultiplexWords * orig_sample_ct;
    uintptr_t* cur_sample_include;
    uint32_t* sample_include_cumulative_popcounts;
    uintptr_t* loadbuf;
    uintptr_t* splitbuf_hom;
    uintptr_t* splitbuf_ref2het;
    // ok if allocations are a bit oversized
    if (bigstack_alloc_ul(raw_sample_ctl, &cur_sample_include) ||
        bigstack_alloc_ui(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_ul(sample_ctaw2, &loadbuf) ||
        bigstack_alloc_ul(kPglBitTransposeBatch * sample_ctaw, &splitbuf_hom) ||
        bigstack_alloc_ul(kPglBitTransposeBatch * sample_ctaw, &splitbuf_ref2het) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_hom[0])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_ref2het[0])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_hom[1])) ||
        bigstack_alloc_ul(king_bufsizew, &(g_smaj_ref2het[1]))) {
      goto calc_king_table_subset_ret_NOMEM;
    }
    // force this to be cacheline-aligned
    vul_t* vecaligned_buf = S_CAST(vul_t*, bigstack_alloc(kPglBitTransposeBufbytes));
    if (!vecaligned_buf) {
      goto calc_king_table_subset_ret_NOMEM;
    }
    set_king_table_fname(king_modifier, parallel_idx, parallel_tot, outname_end);
    uint32_t fname_slen;
#ifdef _WIN32
    fname_slen = GetFullPathName(subset_fname, kPglFnamesize, g_textbuf, nullptr);
    if ((!fname_slen) || (fname_slen > kPglFnamesize))
#else
    if (!realpath(subset_fname, g_textbuf))
#endif
    {
      LOGERRPRINTFWW(g_errstr_fopen, subset_fname);
      goto calc_king_table_subset_ret_OPEN_FAIL;
    }
    if (realpath_identical(outname, g_textbuf, &(g_textbuf[kPglFnamesize + 64]))) {
      logerrprint("Warning: --king-table-subset input filename matches --make-king-table output\nfilename.  Appending '~' to input filename.\n");
      fname_slen = strlen(subset_fname);
      memcpy(g_textbuf, subset_fname, fname_slen);
      memcpy(&(g_textbuf[fname_slen]), "~", 2);
      if (rename(subset_fname, g_textbuf)) {
        logerrprint("Error: Failed to append '~' to --king-table-subset input filename.\n");
        goto calc_king_table_subset_ret_OPEN_FAIL;
      }
      reterr = gzopen_read_checked(g_textbuf, &gz_infile);
    } else {
      reterr = gzopen_read_checked(subset_fname, &gz_infile);
    }
    if (reterr) {
      goto calc_king_table_subset_ret_1;
    }

    // Safe to "write" the header line now, if necessary.
    reterr = cswrite_init2(outname, 0, king_modifier & kfKingTableZs, max_thread_ct, kMaxMediumLine + kCompressStreamBlock, &css, &cswritep);
    if (reterr) {
      goto calc_king_table_subset_ret_1;
    }
    const uint32_t king_col_sid = sid_col_required(orig_sample_include, sids, orig_sample_ct, max_sid_blen, king_modifier / kfKingColMaybesid);
    if (!parallel_idx) {
      cswritep = append_king_table_header(king_modifier, king_col_sid, cswritep);
    }
    if (!sids) {
      max_sid_blen = 2;
    }
    uintptr_t max_sample_augid_blen = max_sample_id_blen;
    if (king_col_sid) {
      max_sample_augid_blen += max_sid_blen;
    }
    char* collapsed_sample_augids;
    if (bigstack_alloc_c(max_sample_augid_blen * orig_sample_ct, &collapsed_sample_augids)) {
      goto calc_king_table_subset_ret_NOMEM;
    }
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    if (calc_thread_ct > orig_sample_ct / 32) {
      calc_thread_ct = orig_sample_ct / 32;
    }
    if (!calc_thread_ct) {
      calc_thread_ct = 1;
    }
    // possible todo: allow this to change between passes
    ts.calc_thread_ct = calc_thread_ct;
    // could eventually have 64-bit g_thread_start?
    if (bigstack_alloc_ui(calc_thread_ct + 1, &g_thread_start) ||
        bigstack_alloc_thread(calc_thread_ct, &ts.threads)) {
      goto calc_king_table_subset_ret_NOMEM;
    }

    g_textbuf[kMaxMediumLine - 1] = ' ';
    if (!gzgets(gz_infile, g_textbuf, kMaxMediumLine)) {
      if (!gzeof(gz_infile)) {
        goto calc_king_table_subset_ret_READ_FAIL;
      }
      logerrprint("Error: Empty --king-table-subset file.\n");
      goto calc_king_table_subset_ret_MALFORMED_INPUT;
    }
    if (!g_textbuf[kMaxMediumLine - 1]) {
      logerrprint("Error: Line 1 of --king-table-subset file is pathologically long.\n");
      goto calc_king_table_subset_ret_MALFORMED_INPUT;
    }
    const char* textbuf_iter = skip_initial_spaces(g_textbuf);
    if (is_eoln_kns(*textbuf_iter)) {
      goto calc_king_table_subset_ret_INVALID_HEADER;
    }
    const char* token_end = token_endnn(textbuf_iter);
    uint32_t token_slen = token_end - textbuf_iter;
    // Make this work with both KING- and plink2-generated .kin0 files.
    if ((!strequal_k(textbuf_iter, "#FID1", token_slen)) && (!strequal_k(textbuf_iter, "FID", token_slen))) {
      goto calc_king_table_subset_ret_INVALID_HEADER;
    }
    textbuf_iter = skip_initial_spaces(token_end);
    token_end = token_endnn(textbuf_iter);
    token_slen = token_end - textbuf_iter;
    if (!strequal_k(textbuf_iter, "ID1", token_slen)) {
      goto calc_king_table_subset_ret_INVALID_HEADER;
    }
    textbuf_iter = skip_initial_spaces(token_end);
    token_end = token_endnn(textbuf_iter);
    token_slen = token_end - textbuf_iter;
    uint32_t id2_skip = 0;
    xid_mode_t xid_mode;
    if (strequal_k(textbuf_iter, "SID1", token_slen)) {
      if (sids) {
        xid_mode = kfXidModeFidiidSid;
      } else {
        id2_skip = 1;
      }
      textbuf_iter = skip_initial_spaces(token_end);
      token_end = token_endnn(textbuf_iter);
      token_slen = token_end - textbuf_iter;
    } else {
      xid_mode = kfXidModeFidiid;
    }
    if (!strequal_k(textbuf_iter, "FID2", token_slen)) {
      goto calc_king_table_subset_ret_INVALID_HEADER;
    }
    textbuf_iter = skip_initial_spaces(token_end);
    token_end = token_endnn(textbuf_iter);
    token_slen = token_end - textbuf_iter;
    if (!strequal_k(textbuf_iter, "ID2", token_slen)) {
      goto calc_king_table_subset_ret_INVALID_HEADER;
    }
    if (xid_mode == kfXidModeFidiidSid) {
      // technically don't need to check this in id2_skip case
      textbuf_iter = skip_initial_spaces(token_end);
      token_end = token_endnn(textbuf_iter);
      token_slen = token_end - textbuf_iter;
      if (!strequal_k(textbuf_iter, "SID2", token_slen)) {
        goto calc_king_table_subset_ret_INVALID_HEADER;
      }
    }
    uint32_t kinship_skip = 0;
    if (king_table_subset_thresh != -DBL_MAX) {
      king_table_subset_thresh *= 1.0 - kSmallEpsilon;
      while (1) {
        textbuf_iter = skip_initial_spaces(token_end);
        token_end = token_endnn(textbuf_iter);
        token_slen = token_end - textbuf_iter;
        if (!token_slen) {
          logerrprint("Error: No kinship-coefficient column in --king-table-subset file.\n");
          goto calc_king_table_subset_ret_INCONSISTENT_INPUT;
        }
        if (strequal_k(textbuf_iter, "KINSHIP", token_slen) || strequal_k(textbuf_iter, "Kinship", token_slen)) {
          break;
        }
        ++kinship_skip;
      }
    }

    uint32_t* xid_map; // IDs not collapsed
    char* sorted_xidbox;
    uintptr_t max_xid_blen;
    reterr = sorted_xidbox_init_alloc(orig_sample_include, sample_ids, sids, orig_sample_ct, max_sample_id_blen, max_sid_blen, 0, xid_mode, 0, &sorted_xidbox, &xid_map, &max_xid_blen);
    if (reterr) {
      goto calc_king_table_subset_ret_1;
    }
    char* idbuf;
    if (bigstack_alloc_c(max_xid_blen, &idbuf)) {
      goto calc_king_table_subset_ret_NOMEM;
    }

    g_homhom_needed = (king_modifier & kfKingColNsnp) || ((!(king_modifier & kfKingCounts)) && (king_modifier & (kfKingColHethet | kfKingColIbs0 | kfKingColIbs1)));
    const uint32_t homhom_needed_p4 = g_homhom_needed + 4;
    // if homhom_needed, 8 + 20 bytes per pair, otherwise 8 + 16
    uintptr_t pair_buf_capacity = bigstack_left();
    if (pair_buf_capacity < 2 * kCacheline) {
      goto calc_king_table_subset_ret_NOMEM;
    }
    // adverse rounding
    pair_buf_capacity = (pair_buf_capacity - 2 * kCacheline) / (24 + 4 * g_homhom_needed);
    if (pair_buf_capacity > 0xffffffffU) {
      // 32-bit g_thread_start[] for now
      pair_buf_capacity = 0xffffffffU;
    }
    g_loaded_sample_idx_pairs = S_CAST(uint32_t*, bigstack_alloc_raw_rd(pair_buf_capacity * 2 * sizeof(int32_t)));
    g_king_counts = R_CAST(uint32_t*, g_bigstack_base);
    uint64_t pair_idx = 0;
    fputs("Scanning --king-table-subset file...", stdout);
    fflush(stdout);
    reterr = king_table_subset_load(sorted_xidbox, xid_map, max_xid_blen, orig_sample_ct, king_table_subset_thresh, xid_mode, id2_skip, kinship_skip, (parallel_tot != 1), 0, pair_buf_capacity, &gz_infile, &pair_idx, g_loaded_sample_idx_pairs, g_textbuf, idbuf);
    if (reterr) {
      goto calc_king_table_subset_ret_1;
    }
    uint64_t pair_idx_global_start = 0;
    uint64_t pair_idx_global_stop = ~0LLU;
    if (parallel_tot != 1) {
      const uint64_t parallel_pair_ct = pair_idx;
      pair_idx_global_start = (parallel_idx * parallel_pair_ct) / parallel_tot;
      pair_idx_global_stop = ((parallel_idx + 1) * parallel_pair_ct) / parallel_tot;
      if (pair_idx > pair_buf_capacity) {
        // may as well document possible overflow
        if (parallel_pair_ct > ((~0LLU) / kParallelMax)) {
          logerrprint("Error: Too many --king-table-subset sample pairs for this " PROG_NAME_STR " build.\n");
          reterr = kPglRetNotYetSupported;
          goto calc_king_table_subset_ret_1;
        }
        if (pair_idx_global_stop > pair_buf_capacity) {
          // large --parallel job
          gzrewind(gz_infile);
          if (!gzgets(gz_infile, g_textbuf, kMaxMediumLine)) {
            goto calc_king_table_subset_ret_READ_FAIL;
          }
          reterr = king_table_subset_load(sorted_xidbox, xid_map, max_xid_blen, orig_sample_ct, king_table_subset_thresh, xid_mode, id2_skip, kinship_skip, 0, pair_idx_global_start, MINV(pair_idx_global_stop, pair_idx_global_start + pair_buf_capacity), &gz_infile, &pair_idx, g_loaded_sample_idx_pairs, g_textbuf, idbuf);
          if (reterr) {
            goto calc_king_table_subset_ret_1;
          }
        } else {
          pair_idx = pair_idx_global_stop;
          if (pair_idx_global_start) {
            memmove(g_loaded_sample_idx_pairs, &(g_loaded_sample_idx_pairs[pair_idx_global_start * 2]), (pair_idx_global_stop - pair_idx_global_start) * 2 * sizeof(int32_t));
          }
        }
      } else {
        pair_idx = pair_idx_global_stop;
        if (pair_idx_global_start) {
          memmove(g_loaded_sample_idx_pairs, &(g_loaded_sample_idx_pairs[pair_idx_global_start * 2]), (pair_idx_global_stop - pair_idx_global_start) * 2 * sizeof(int32_t));
        }
      }
    }
    uint64_t pair_idx_cur_start = pair_idx_global_start;
    uint64_t king_table_filter_ct = 0;
    uintptr_t pass_idx = 1;
    while (pair_idx_cur_start < pair_idx) {
      fill_ulong_zero(raw_sample_ctl, cur_sample_include);
      const uintptr_t cur_pair_ct = pair_idx - pair_idx_cur_start;
      const uintptr_t cur_pair_ct_x2 = 2 * cur_pair_ct;
      for (uintptr_t ulii = 0; ulii < cur_pair_ct_x2; ++ulii) {
        set_bit(g_loaded_sample_idx_pairs[ulii], cur_sample_include);
      }
      fill_cumulative_popcounts(cur_sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
      const uint32_t cur_sample_ct = sample_include_cumulative_popcounts[raw_sample_ctl - 1] + popcount_long(cur_sample_include[raw_sample_ctl - 1]);
      const uint32_t cur_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(cur_sample_ct);
      const uint32_t cur_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(cur_sample_ct);
      if (cur_sample_ct != raw_sample_ct) {
        for (uintptr_t ulii = 0; ulii < cur_pair_ct_x2; ++ulii) {
          g_loaded_sample_idx_pairs[ulii] = raw_to_subsetted_pos(cur_sample_include, sample_include_cumulative_popcounts, g_loaded_sample_idx_pairs[ulii]);
        }
      }
      fill_uint_zero(cur_pair_ct * homhom_needed_p4, g_king_counts);
      char* sample_augids_iter = collapsed_sample_augids;
      uint32_t sample_uidx = 0;
      for (uint32_t sample_idx = 0; sample_idx < cur_sample_ct; ++sample_idx, ++sample_uidx) {
        next_set_unsafe_ck(cur_sample_include, &sample_uidx);
        char* write_iter = strcpya(sample_augids_iter, &(sample_ids[sample_uidx * max_sample_id_blen]));
        if (king_col_sid) {
          *write_iter++ = '\t';
          if (sids) {
            strcpy(write_iter, &(sids[sample_uidx * max_sid_blen]));
          } else {
            memcpy(write_iter, "0", 2);
          }
        } else {
          *write_iter = '\0';
        }
        sample_augids_iter = &(sample_augids_iter[max_sample_augid_blen]);
      }
      for (uint32_t tidx = 0; tidx <= calc_thread_ct; ++tidx) {
        g_thread_start[tidx] = (tidx * S_CAST(uint64_t, cur_pair_ct)) / calc_thread_ct;
      }
      if (pass_idx != 1) {
        reinit_threads3z(&ts);
      }
      uint32_t variant_uidx = 0;
      uint32_t variants_completed = 0;
      uint32_t parity = 0;
      const uint32_t sample_batch_ct_m1 = (cur_sample_ct - 1) / kPglBitTransposeBatch;
      pgr_clear_ld_cache(simple_pgrp);
      do {
        const uint32_t cur_block_size = MINV(variant_ct - variants_completed, kKingMultiplex);
        uintptr_t* cur_smaj_hom = g_smaj_hom[parity];
        uintptr_t* cur_smaj_ref2het = g_smaj_ref2het[parity];
        uint32_t write_batch_idx = 0;
        // "block" = distance computation granularity, usually 1024 or 1536
        //           variants
        // "batch" = variant-major-to-sample-major transpose granularity,
        //           currently 512 variants
        uint32_t variant_batch_size = kPglBitTransposeBatch;
        uint32_t variant_batch_size_rounded_up = kPglBitTransposeBatch;
        const uint32_t write_batch_ct_m1 = (cur_block_size - 1) / kPglBitTransposeBatch;
        while (1) {
          if (write_batch_idx >= write_batch_ct_m1) {
            if (write_batch_idx > write_batch_ct_m1) {
              break;
            }
            variant_batch_size = MOD_NZ(cur_block_size, kPglBitTransposeBatch);
            variant_batch_size_rounded_up = variant_batch_size;
            const uint32_t variant_batch_size_rem = variant_batch_size % kBitsPerWord;
            if (variant_batch_size_rem) {
              const uint32_t trailing_variant_ct = kBitsPerWord - variant_batch_size_rem;
              variant_batch_size_rounded_up += trailing_variant_ct;
              fill_ulong_zero(trailing_variant_ct * cur_sample_ctaw, &(splitbuf_hom[variant_batch_size * cur_sample_ctaw]));
              fill_ulong_zero(trailing_variant_ct * cur_sample_ctaw, &(splitbuf_ref2het[variant_batch_size * cur_sample_ctaw]));
            }
          }
          uintptr_t* hom_iter = splitbuf_hom;
          uintptr_t* ref2het_iter = splitbuf_ref2het;
          for (uint32_t uii = 0; uii < variant_batch_size; ++uii, ++variant_uidx) {
            next_set_unsafe_ck(variant_include, &variant_uidx);
            reterr = pgr_read_refalt1_genovec_subset_unsafe(cur_sample_include, sample_include_cumulative_popcounts, cur_sample_ct, variant_uidx, simple_pgrp, loadbuf);
            if (reterr) {
              goto calc_king_table_subset_ret_PGR_FAIL;
            }
            set_trailing_quaters(cur_sample_ct, loadbuf);
            split_hom_ref2het_unsafew(loadbuf, cur_sample_ctaw2, hom_iter, ref2het_iter);
            hom_iter = &(hom_iter[cur_sample_ctaw]);
            ref2het_iter = &(ref2het_iter[cur_sample_ctaw]);
          }
          // uintptr_t* read_iter = loadbuf;
          uintptr_t* write_hom_iter = &(cur_smaj_hom[write_batch_idx * kPglBitTransposeWords]);
          uintptr_t* write_ref2het_iter = &(cur_smaj_ref2het[write_batch_idx * kPglBitTransposeWords]);
          uint32_t sample_batch_idx = 0;
          uint32_t write_batch_size = kPglBitTransposeBatch;
          while (1) {
            if (sample_batch_idx >= sample_batch_ct_m1) {
              if (sample_batch_idx > sample_batch_ct_m1) {
                break;
              }
              write_batch_size = MOD_NZ(cur_sample_ct, kPglBitTransposeBatch);
            }
            // bugfix: read_batch_size must be rounded up to word boundary,
            // since we want to one-out instead of zero-out the trailing bits
            //
            // bugfix: if we always use kPglBitTransposeBatch instead of
            // variant_batch_size_rounded_up, we read/write past the
            // kKingMultiplex limit and clobber the first variants of the next
            // sample with garbage.
            transpose_bitblock(&(splitbuf_hom[sample_batch_idx * kPglBitTransposeWords]), cur_sample_ctaw, kKingMultiplexWords, variant_batch_size_rounded_up, write_batch_size, write_hom_iter, vecaligned_buf);
            transpose_bitblock(&(splitbuf_ref2het[sample_batch_idx * kPglBitTransposeWords]), cur_sample_ctaw, kKingMultiplexWords, variant_batch_size_rounded_up, write_batch_size, write_ref2het_iter, vecaligned_buf);
            ++sample_batch_idx;
            write_hom_iter = &(write_hom_iter[kKingMultiplex * kPglBitTransposeWords]);
            write_ref2het_iter = &(write_ref2het_iter[kKingMultiplex * kPglBitTransposeWords]);
          }
          ++write_batch_idx;
        }
        const uint32_t cur_block_sizew = BITCT_TO_WORDCT(cur_block_size);
        if (cur_block_sizew < kKingMultiplexWords) {
          uintptr_t* write_hom_iter = &(cur_smaj_hom[cur_block_sizew]);
          uintptr_t* write_ref2het_iter = &(cur_smaj_ref2het[cur_block_sizew]);
          const uint32_t write_word_ct = kKingMultiplexWords - cur_block_sizew;
          for (uint32_t sample_idx = 0; sample_idx < cur_sample_ct; ++sample_idx) {
            fill_ulong_zero(write_word_ct, write_hom_iter);
            fill_ulong_zero(write_word_ct, write_ref2het_iter);
            write_hom_iter = &(write_hom_iter[kKingMultiplexWords]);
            write_ref2het_iter = &(write_ref2het_iter[kKingMultiplexWords]);
          }
        }
        if (variants_completed) {
          join_threads3z(&ts);
        } else {
          ts.thread_func_ptr = calc_king_table_subset_thread;
        }
        // this update must occur after join_threads3z() call
        ts.is_last_block = (variants_completed + cur_block_size == variant_ct);
        if (spawn_threads3z(variants_completed, &ts)) {
          goto calc_king_table_subset_ret_THREAD_CREATE_FAIL;
        }
        printf("\r--make-king-table pass %" PRIuPTR ": %u variants complete.", pass_idx, variants_completed);
        fflush(stdout);
        variants_completed += cur_block_size;
        parity = 1 - parity;
      } while (!ts.is_last_block);
      join_threads3z(&ts);
      printf("\r--make-king-table pass %" PRIuPTR ": Writing...                   \b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b", pass_idx);
      fflush(stdout);

      const uint32_t king_col_id = king_modifier & kfKingColId;
      const uint32_t king_col_nsnp = king_modifier & kfKingColNsnp;
      const uint32_t king_col_hethet = king_modifier & kfKingColHethet;
      const uint32_t king_col_ibs0 = king_modifier & kfKingColIbs0;
      const uint32_t king_col_ibs1 = king_modifier & kfKingColIbs1;
      const uint32_t king_col_kinship = king_modifier & kfKingColKinship;
      const uint32_t report_counts = king_modifier & kfKingCounts;
      uint32_t* results_iter = g_king_counts;
      double nonmiss_recip = 0.0;
      for (uintptr_t cur_pair_idx = 0; cur_pair_idx < cur_pair_ct; ++cur_pair_idx, results_iter = &(results_iter[homhom_needed_p4])) {
        const uint32_t ibs0_ct = results_iter[0];
        const uint32_t hethet_ct = results_iter[1];
        const uint32_t het2hom1_ct = results_iter[2];
        const uint32_t het1hom2_ct = results_iter[3];
        const intptr_t smaller_het_ct = hethet_ct + MINV(het1hom2_ct, het2hom1_ct);
        const double kinship_coeff = 0.5 - (S_CAST(double, 4 * S_CAST(intptr_t, ibs0_ct) + het1hom2_ct + het2hom1_ct) / S_CAST(double, 4 * smaller_het_ct));
        if ((king_table_filter != -DBL_MAX) && (kinship_coeff < king_table_filter)) {
          ++king_table_filter_ct;
          continue;
        }
        const uint32_t sample_idx1 = g_loaded_sample_idx_pairs[2 * cur_pair_idx];
        const uint32_t sample_idx2 = g_loaded_sample_idx_pairs[2 * cur_pair_idx + 1];
        if (king_col_id) {
          cswritep = strcpyax(cswritep, &(collapsed_sample_augids[max_sample_augid_blen * sample_idx1]), '\t');
          cswritep = strcpyax(cswritep, &(collapsed_sample_augids[max_sample_augid_blen * sample_idx2]), '\t');
        }
        if (homhom_needed_p4 == 5) {
          const uint32_t homhom_ct = results_iter[4];
          const uint32_t nonmiss_ct = het1hom2_ct + het2hom1_ct + homhom_ct + hethet_ct;
          if (king_col_nsnp) {
            cswritep = uint32toa_x(nonmiss_ct, '\t', cswritep);
          }
          if (!report_counts) {
            nonmiss_recip = 1.0 / u31tod(nonmiss_ct);
          }
        }
        if (king_col_hethet) {
          if (report_counts) {
            cswritep = uint32toa(hethet_ct, cswritep);
          } else {
            cswritep = dtoa_g(nonmiss_recip * u31tod(hethet_ct), cswritep);
          }
          *cswritep++ = '\t';
        }
        if (king_col_ibs0) {
          if (report_counts) {
            cswritep = uint32toa(ibs0_ct, cswritep);
          } else {
            cswritep = dtoa_g(nonmiss_recip * u31tod(ibs0_ct), cswritep);
          }
          *cswritep++ = '\t';
        }
        if (king_col_ibs1) {
          if (report_counts) {
            cswritep = uint32toa_x(het1hom2_ct, '\t', cswritep);
            cswritep = uint32toa(het2hom1_ct, cswritep);
          } else {
            cswritep = dtoa_g(nonmiss_recip * u31tod(het1hom2_ct), cswritep);
            *cswritep++ = '\t';
            cswritep = dtoa_g(nonmiss_recip * u31tod(het2hom1_ct), cswritep);
          }
          *cswritep++ = '\t';
        }
        if (king_col_kinship) {
          cswritep = dtoa_g(kinship_coeff, cswritep);
          ++cswritep;
        }
        decr_append_binary_eoln(&cswritep);
        if (cswrite(&css, &cswritep)) {
          goto calc_king_table_subset_ret_WRITE_FAIL;
        }
      }

      putc_unlocked('\r', stdout);
      const uint64_t pair_complete_ct = pair_idx - pair_idx_global_start;
      LOGPRINTF("Subsetted --make-king-table: %" PRIu64 " pair%s complete.\n", pair_complete_ct, (pair_complete_ct == 1)? "" : "s");
      if (gzeof(gz_infile)) {
        break;
      }
      pair_idx_cur_start = pair_idx;
      fputs("Scanning --king-table-subset file...", stdout);
      fflush(stdout);
      reterr = king_table_subset_load(sorted_xidbox, xid_map, max_xid_blen, orig_sample_ct, king_table_subset_thresh, xid_mode, id2_skip, kinship_skip, 0, pair_idx_cur_start, MINV(pair_idx_global_stop, pair_idx_cur_start + pair_buf_capacity), &gz_infile, &pair_idx, g_loaded_sample_idx_pairs, g_textbuf, idbuf);
      if (reterr) {
        goto calc_king_table_subset_ret_1;
      }
      ++pass_idx;
    }
    if (cswrite_close_null(&css, cswritep)) {
      goto calc_king_table_subset_ret_WRITE_FAIL;
    }
    LOGPRINTFWW("Results written to %s .\n", outname);
    if (king_table_filter != -DBL_MAX) {
      const uint64_t reported_ct = pair_idx - pair_idx_global_start - king_table_filter_ct;
      LOGPRINTF("--king-table-filter: %" PRIu64 " relationship%s reported (%" PRIu64 " filtered out).\n", reported_ct, (reported_ct == 1)? "" : "s", king_table_filter_ct);
    }
  }
  while (0) {
  calc_king_table_subset_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  calc_king_table_subset_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  calc_king_table_subset_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  calc_king_table_subset_ret_PGR_FAIL:
    if (reterr != kPglRetReadFail) {
      logerrprint("Error: Malformed .pgen file.\n");
    }
    break;
  calc_king_table_subset_ret_WRITE_FAIL:
    reterr = kPglRetReadFail;
    break;
  calc_king_table_subset_ret_INVALID_HEADER:
    logerrprint("Error: Invalid header line in --king-table-subset file.\n");
  calc_king_table_subset_ret_MALFORMED_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  calc_king_table_subset_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  calc_king_table_subset_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 calc_king_table_subset_ret_1:
  threads3z_cleanup(&ts, nullptr);
  gzclose_cond(gz_infile);
  cswrite_close_cond(&css, cswritep);
  fclose_cond(outfile);
  bigstack_reset(bigstack_mark);
  return reterr;
}

// this probably belongs in plink2_common
void expand_variant_dosages(const uintptr_t* genovec, const uintptr_t* dosage_present, const dosage_t* dosage_vals, double slope, double intercept, double missing_val, uint32_t sample_ct, uint32_t dosage_ct, double* expanded_dosages) {
  double lookup_vals[4];
  lookup_vals[0] = intercept;
  lookup_vals[1] = intercept + slope;
  lookup_vals[2] = intercept + 2 * slope;
  lookup_vals[3] = missing_val;
  const uintptr_t* genovec_iter = genovec;
  const uint32_t sample_ctl2_m1 = (sample_ct - 1) / kBitsPerWordD2;
  uint32_t widx = 0;
  uint32_t loop_len = kBitsPerWordD2;
  double* expanded_dosages_iter = expanded_dosages;
  while (1) {
    if (widx >= sample_ctl2_m1) {
      if (widx > sample_ctl2_m1) {
        break;
      }
      loop_len = MOD_NZ(sample_ct, kBitsPerWordD2);
    }
    uintptr_t geno_word = *genovec_iter++;
    for (uint32_t uii = 0; uii < loop_len; ++uii) {
      *expanded_dosages_iter++ = lookup_vals[geno_word & 3];
      geno_word >>= 2;
    }
    ++widx;
  }
  if (dosage_ct) {
    slope *= kRecipDosageMid;
    uint32_t sample_uidx = 0;
    for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
      next_set_unsafe_ck(dosage_present, &sample_uidx);
      expanded_dosages[sample_uidx] = dosage_vals[dosage_idx] * slope + intercept;
    }
  }
}

// assumes trailing bits of genovec are zeroed out
pglerr_t expand_centered_varmaj(const uintptr_t* genovec, const uintptr_t* dosage_present, const dosage_t* dosage_vals, uint32_t variance_standardize, uint32_t sample_ct, uint32_t dosage_ct, double maj_freq, double* normed_dosages) {
  const double nonmaj_freq = 1.0 - maj_freq;
  double inv_stdev;
  if (variance_standardize) {
    const double variance = 2 * maj_freq * nonmaj_freq;
    if (variance < kSmallEpsilon) {
      uint32_t genocounts[4];
      genovec_count_freqs_unsafe(genovec, sample_ct, genocounts);
      if (dosage_ct || genocounts[1] || genocounts[2]) {
        return kPglRetInconsistentInput;
      }
      fill_double_zero(sample_ct, normed_dosages);
      return kPglRetSuccess;
    }
    inv_stdev = 1.0 / sqrt(variance);
  } else {
    inv_stdev = 1.0;
  }
  expand_variant_dosages(genovec, dosage_present, dosage_vals, inv_stdev, -2 * nonmaj_freq * inv_stdev, 0.0, sample_ct, dosage_ct, normed_dosages);
  return kPglRetSuccess;
}

pglerr_t load_centered_varmaj(const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, uint32_t variance_standardize, uint32_t sample_ct, uint32_t variant_uidx, alt_allele_ct_t maj_allele_idx, double maj_freq, pgen_reader_t* simple_pgrp, uint32_t* missing_presentp, double* normed_dosages, uintptr_t* genovec_buf, uintptr_t* dosage_present_buf, dosage_t* dosage_vals_buf) {
  // todo: multiallelic case
  uint32_t dosage_ct;
  uint32_t is_explicit_alt1;
  pglerr_t reterr = pgr_read_refalt1_genovec_dosage16_subset_unsafe(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec_buf, dosage_present_buf, dosage_vals_buf, &dosage_ct, &is_explicit_alt1);
  if (reterr) {
    // don't print malformed-.pgen error message here for now, since we may
    // want to put this in a multithreaded loop?
    return reterr;
  }
  if (maj_allele_idx) {
    genovec_invert_unsafe(sample_ct, genovec_buf);
    if (dosage_ct) {
      biallelic_dosage16_invert(dosage_ct, dosage_vals_buf);
    }
  }
  zero_trailing_quaters(sample_ct, genovec_buf);
  if (missing_presentp) {
    // missing_present assumed to be initialized to 0
    const uint32_t sample_ctl2 = QUATERCT_TO_WORDCT(sample_ct);
    if (!dosage_ct) {
      for (uint32_t widx = 0; widx < sample_ctl2; ++widx) {
        const uintptr_t genovec_word = genovec_buf[widx];
        if (genovec_word & (genovec_word >> 1) & kMask5555) {
          *missing_presentp = 1;
          break;
        }
      }
    } else {
      halfword_t* dosage_present_alias = R_CAST(halfword_t*, dosage_present_buf);
      for (uint32_t widx = 0; widx < sample_ctl2; ++widx) {
        const uintptr_t genovec_word = genovec_buf[widx];
        const uintptr_t ulii = genovec_word & (genovec_word >> 1) & kMask5555;
        if (ulii) {
          if (pack_word_to_halfword(ulii) & (~dosage_present_alias[widx])) {
            *missing_presentp = 1;
            break;
          }
        }
      }
    }
  }
  return expand_centered_varmaj(genovec_buf, dosage_present_buf, dosage_vals_buf, variance_standardize, sample_ct, dosage_ct, maj_freq, normed_dosages);
}

// multithread globals
double* g_normed_dosage_vmaj_bufs[2] = {nullptr, nullptr};
double* g_normed_dosage_smaj_bufs[2] = {nullptr, nullptr};

double* g_grm = nullptr;

static uint32_t g_pca_sample_ct = 0;
static uint32_t g_cur_batch_size = 0;

CONSTU31(kGrmVariantBlockSize, 144);

// turns out dsyrk_ does exactly what we want here
THREAD_FUNC_DECL calc_grm_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  assert(!tidx);
  const uint32_t sample_ct = g_pca_sample_ct;
  double* grm = g_grm;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (cur_batch_size) {
      transpose_multiply_self_incr(g_normed_dosage_vmaj_bufs[parity], sample_ct, cur_batch_size, grm);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

// can't use dsyrk_, so we manually partition the GRM piece we need to compute
// into an appropriate number of sub-pieces
THREAD_FUNC_DECL calc_grm_part_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uintptr_t sample_ct = g_pca_sample_ct;
  const uintptr_t first_thread_row_start_idx = g_thread_start[0];
  const uintptr_t row_start_idx = g_thread_start[tidx];
  const uintptr_t row_ct = g_thread_start[tidx + 1] - row_start_idx;
  double* grm_piece = &(g_grm[(row_start_idx - first_thread_row_start_idx) * sample_ct]);
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uintptr_t cur_batch_size = g_cur_batch_size;
    if (cur_batch_size) {
      double* normed_vmaj = g_normed_dosage_vmaj_bufs[parity];
      double* normed_smaj = g_normed_dosage_smaj_bufs[parity];
      row_major_matrix_multiply_incr(&(normed_smaj[row_start_idx * cur_batch_size]), normed_vmaj, row_ct, sample_ct, cur_batch_size, grm_piece);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

// missing_nz bit is set iff that sample has at least one missing entry in
// current block
uintptr_t* g_missing_nz[2] = {nullptr, nullptr};
uintptr_t* g_missing_smaj[2] = {nullptr, nullptr};
uint32_t* g_missing_dbl_exclude_cts = nullptr;

CONSTU31(kDblMissingBlockWordCt, 2);
CONSTU31(kDblMissingBlockSize, kDblMissingBlockWordCt * kBitsPerWord);

THREAD_FUNC_DECL calc_dbl_missing_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint64_t first_thread_row_start_idx = g_thread_start[0];
  const uint64_t dbl_exclude_offset = (first_thread_row_start_idx * (first_thread_row_start_idx - 1)) / 2;
  const uint32_t row_start_idx = g_thread_start[tidx];
  const uintptr_t row_end_idx = g_thread_start[tidx + 1];
  uint32_t* missing_dbl_exclude_cts = g_missing_dbl_exclude_cts;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;

    // currently only care about zero vs. nonzero (I/O error)
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (cur_batch_size) {
      const uintptr_t* missing_nz = g_missing_nz[parity];
      const uintptr_t* missing_smaj = g_missing_smaj[parity];
      const uint32_t first_idx = next_set(missing_nz, 0, row_end_idx);
      uint32_t sample_idx = first_idx;
      uint32_t prev_missing_nz_ct = 0;
      if (sample_idx < row_start_idx) {
        sample_idx = next_set(missing_nz, row_start_idx, row_end_idx);
        if (sample_idx != row_end_idx) {
          prev_missing_nz_ct = popcount_bit_idx(missing_nz, 0, row_start_idx);
        }
      }
      while (sample_idx < row_end_idx) {
        uint32_t sample_idx2 = first_idx;
        // todo: compare this explicit unroll with ordinary iteration over a
        // cur_words[] array
        // todo: try 1 word at a time, and 30 words at a time
        const uintptr_t cur_word0 = missing_smaj[sample_idx * kDblMissingBlockWordCt];
        const uintptr_t cur_word1 = missing_smaj[sample_idx * kDblMissingBlockWordCt + 1];
#ifndef __LP64__
        const uintptr_t cur_word2 = missing_smaj[sample_idx * kDblMissingBlockWordCt + 2];
        const uintptr_t cur_word3 = missing_smaj[sample_idx * kDblMissingBlockWordCt + 3];
#endif
        // (sample_idx - 1) underflow ok
        uint32_t* write_base = &(missing_dbl_exclude_cts[((S_CAST(uint64_t, sample_idx) * (sample_idx - 1)) / 2) - dbl_exclude_offset]);
        for (uint32_t uii = 0; uii < prev_missing_nz_ct; ++uii, ++sample_idx2) {
          next_set_unsafe_ck(missing_nz, &sample_idx2);
          const uintptr_t* cur_missing_smaj_base = &(missing_smaj[sample_idx2 * kDblMissingBlockWordCt]);
          const uintptr_t cur_and0 = cur_word0 & cur_missing_smaj_base[0];
          const uintptr_t cur_and1 = cur_word1 & cur_missing_smaj_base[1];
#ifdef __LP64__
          if (cur_and0 || cur_and1) {
            write_base[sample_idx2] += popcount_2_longs(cur_and0, cur_and1);
          }
#else
          const uintptr_t cur_and2 = cur_word2 & cur_missing_smaj_base[2];
          const uintptr_t cur_and3 = cur_word3 & cur_missing_smaj_base[3];
          if (cur_and0 || cur_and1 || cur_and2 || cur_and3) {
            write_base[sample_idx2] += popcount_4_longs(cur_and0, cur_and1, cur_and2, cur_and3);
          }
#endif
        }
        ++prev_missing_nz_ct;
        sample_idx = next_set(missing_nz, sample_idx + 1, row_end_idx);
      }
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

pglerr_t calc_missing_matrix(const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, const uintptr_t* variant_include, uint32_t sample_ct, uint32_t variant_ct, uint32_t parallel_idx, uint32_t parallel_tot, uint32_t row_start_idx, uintptr_t row_end_idx, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, uint32_t** missing_cts_ptr, uint32_t** missing_dbl_exclude_cts_ptr) {
  unsigned char* bigstack_mark = g_bigstack_base;
  threads_state_t ts;
  init_threads3z(&ts);
  pglerr_t reterr = kPglRetSuccess;
  {
    const uintptr_t row_end_idxl = BITCT_TO_WORDCT(row_end_idx);
    // bugfix (1 Oct 2017): missing_vmaj rows must be vector-aligned
    const uintptr_t row_end_idxaw = BITCT_TO_ALIGNED_WORDCT(row_end_idx);
    uintptr_t* missing_vmaj = nullptr;
    uintptr_t* genovec_buf = nullptr;
    if (bigstack_calloc_ui(row_end_idx, missing_cts_ptr) ||
        bigstack_calloc_ui((S_CAST(uint64_t, row_end_idx) * (row_end_idx - 1) - S_CAST(uint64_t, row_start_idx) * (row_start_idx - 1)) / 2, missing_dbl_exclude_cts_ptr) ||
        bigstack_calloc_ul(row_end_idxl, &g_missing_nz[0]) ||
        bigstack_calloc_ul(row_end_idxl, &g_missing_nz[1]) ||
        bigstack_alloc_ul(QUATERCT_TO_WORDCT(row_end_idx), &genovec_buf) ||
        bigstack_alloc_ul(row_end_idxaw * (k1LU * kDblMissingBlockSize), &missing_vmaj) ||
        bigstack_alloc_ul(round_up_pow2(row_end_idx, 2) * kDblMissingBlockWordCt, &g_missing_smaj[0]) ||
        bigstack_alloc_ul(round_up_pow2(row_end_idx, 2) * kDblMissingBlockWordCt, &g_missing_smaj[1])) {
      goto calc_missing_matrix_ret_NOMEM;
    }
    uint32_t* missing_cts = *missing_cts_ptr;
    uint32_t* missing_dbl_exclude_cts = *missing_dbl_exclude_cts_ptr;
    g_missing_dbl_exclude_cts = missing_dbl_exclude_cts;
    vul_t* transpose_bitblock_wkspace = S_CAST(vul_t*, bigstack_alloc_raw(kPglBitTransposeBufbytes));
    uint32_t calc_thread_ct = (max_thread_ct > 8)? (max_thread_ct - 1) : max_thread_ct;
    ts.calc_thread_ct = calc_thread_ct;
    if (bigstack_alloc_ui(calc_thread_ct + 1, &g_thread_start) ||
        bigstack_alloc_thread(calc_thread_ct, &ts.threads)) {
      goto calc_missing_matrix_ret_NOMEM;
    }
    // note that this g_thread_start[] may have different values than the one
    // computed by calc_grm(), since calc_thread_ct changes in the MTBLAS and
    // OS X cases.
    triangle_fill(sample_ct, calc_thread_ct, parallel_idx, parallel_tot, 0, 1, g_thread_start);
    assert(g_thread_start[0] == row_start_idx);
    assert(g_thread_start[calc_thread_ct] == row_end_idx);
    const uint32_t sample_transpose_batch_ct_m1 = (row_end_idx - 1) / kPglBitTransposeBatch;

    uint32_t parity = 0;
    uint32_t cur_variant_idx_start = 0;
    uint32_t variant_uidx = 0;
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    // caller's responsibility to print this
    // logprint("Correcting for missingness: ");
    fputs("0%", stdout);
    fflush(stdout);
    pgr_clear_ld_cache(simple_pgrp);
    while (1) {
      uint32_t cur_batch_size = 0;
      if (!ts.is_last_block) {
        cur_batch_size = kDblMissingBlockSize;
        uint32_t cur_variant_idx_end = cur_variant_idx_start + cur_batch_size;
        if (cur_variant_idx_end > variant_ct) {
          cur_batch_size = variant_ct - cur_variant_idx_start;
          cur_variant_idx_end = variant_ct;
          fill_ulong_zero((kDblMissingBlockSize - cur_batch_size) * row_end_idxaw, &(missing_vmaj[cur_batch_size * row_end_idxaw]));
        }
        uintptr_t* missing_vmaj_iter = missing_vmaj;
        for (uint32_t variant_idx = cur_variant_idx_start; variant_idx < cur_variant_idx_end; ++variant_uidx, ++variant_idx) {
          next_set_unsafe_ck(variant_include, &variant_uidx);
          reterr = pgr_read_missingness_multi(sample_include, sample_include_cumulative_popcounts, row_end_idx, variant_uidx, simple_pgrp, nullptr, missing_vmaj_iter, nullptr, genovec_buf);
          if (reterr) {
            if (reterr == kPglRetMalformedInput) {
              logprint("\n");
              logerrprint("Error: Malformed .pgen file.\n");
            }
            goto calc_missing_matrix_ret_1;
          }
          missing_vmaj_iter = &(missing_vmaj_iter[row_end_idxaw]);
        }
        uintptr_t* cur_missing_smaj_iter = g_missing_smaj[parity];
        uint32_t sample_transpose_batch_idx = 0;
        uint32_t sample_batch_size = kPglBitTransposeBatch;
        while (1) {
          if (sample_transpose_batch_idx >= sample_transpose_batch_ct_m1) {
            if (sample_transpose_batch_idx > sample_transpose_batch_ct_m1) {
              break;
            }
            sample_batch_size = MOD_NZ(row_end_idx, kPglBitTransposeBatch);
          }
          // missing_smaj offset needs to be 64-bit if kDblMissingBlockWordCt
          // increases
          transpose_bitblock(&(missing_vmaj[sample_transpose_batch_idx * kPglBitTransposeWords]), row_end_idxaw, kDblMissingBlockWordCt, kDblMissingBlockSize, sample_batch_size, &(cur_missing_smaj_iter[sample_transpose_batch_idx * kPglBitTransposeBatch * kDblMissingBlockWordCt]), transpose_bitblock_wkspace);
          ++sample_transpose_batch_idx;
        }
        uintptr_t* cur_missing_nz = g_missing_nz[parity];
        fill_ulong_zero(row_end_idxl, cur_missing_nz);
        for (uint32_t sample_idx = 0; sample_idx < row_end_idx; ++sample_idx) {
          const uintptr_t cur_word0 = *cur_missing_smaj_iter++;
          const uintptr_t cur_word1 = *cur_missing_smaj_iter++;
#ifdef __LP64__
          if (cur_word0 || cur_word1) {
            set_bit(sample_idx, cur_missing_nz);
            missing_cts[sample_idx] += popcount_2_longs(cur_word0, cur_word1);
          }
#else
          const uintptr_t cur_word2 = *cur_missing_smaj_iter++;
          const uintptr_t cur_word3 = *cur_missing_smaj_iter++;
          if (cur_word0 || cur_word1 || cur_word2 || cur_word3) {
            set_bit(sample_idx, cur_missing_nz);
            missing_cts[sample_idx] += popcount_4_longs(cur_word0, cur_word1, cur_word2, cur_word3);
          }
#endif
        }
      }
      if (cur_variant_idx_start) {
        join_threads3z(&ts);
        if (ts.is_last_block) {
          break;
        }
        if (cur_variant_idx_start >= next_print_variant_idx) {
          if (pct > 10) {
            putc_unlocked('\b', stdout);
          }
          pct = (cur_variant_idx_start * 100LLU) / variant_ct;
          printf("\b\b%u%%", pct++);
          fflush(stdout);
          next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
        }
      }
      ts.is_last_block = (cur_variant_idx_start + cur_batch_size == variant_ct);
      g_cur_batch_size = cur_batch_size;
      ts.thread_func_ptr = calc_dbl_missing_thread;
      if (spawn_threads3z(cur_variant_idx_start, &ts)) {
        goto calc_missing_matrix_ret_THREAD_CREATE_FAIL;
      }
      cur_variant_idx_start += cur_batch_size;
      parity = 1 - parity;
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprint("done.\n");
    bigstack_mark = R_CAST(unsigned char*, g_missing_nz[0]);
  }
  while (0) {
  calc_missing_matrix_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  calc_missing_matrix_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 calc_missing_matrix_ret_1:
  threads3z_cleanup(&ts, &g_cur_batch_size);
  bigstack_reset(bigstack_mark);
  return reterr;
}

pglerr_t calc_grm(const uintptr_t* orig_sample_include, const char* sample_ids, const char* sids, uintptr_t* variant_include, const chr_info_t* cip, const uintptr_t* variant_allele_idxs, const alt_allele_ct_t* maj_alleles, const double* allele_freqs, uint32_t raw_sample_ct, uint32_t sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, uint32_t raw_variant_ct, uint32_t variant_ct, grm_flags_t grm_flags, uint32_t parallel_idx, uint32_t parallel_tot, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, char* outname, char* outname_end, double** grm_ptr) {
  unsigned char* bigstack_mark = g_bigstack_base;
  unsigned char* bigstack_end_mark = g_bigstack_end;
  FILE* outfile = nullptr;
  char* cswritep = nullptr;
  compress_stream_state_t css;
  threads_state_t ts;
  pglerr_t reterr = kPglRetSuccess;
  cswrite_init_null(&css);
  init_threads3z(&ts);
  {
    if (sample_ct < 2) {
      logerrprint("Error: GRM construction requires at least two samples.\n");
      goto calc_grm_ret_INCONSISTENT_INPUT;
    }
    assert(variant_ct);
#if defined(__APPLE__) || defined(USE_MTBLAS)
    uint32_t calc_thread_ct = 1;
#else
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    if (calc_thread_ct * parallel_tot > sample_ct / 32) {
      calc_thread_ct = sample_ct / (32 * parallel_tot);
      if (!calc_thread_ct) {
        calc_thread_ct = 1;
      }
    }
#endif
    ts.calc_thread_ct = calc_thread_ct;
    const uintptr_t* sample_include = orig_sample_include;
    const uint32_t raw_sample_ctl = BITCT_TO_WORDCT(raw_sample_ct);
    uint32_t row_start_idx = 0;
    uintptr_t row_end_idx = sample_ct;
    uint32_t* thread_start = nullptr;
    if ((calc_thread_ct != 1) || (parallel_tot != 1)) {
      // note that grm should be allocated on bottom if no --parallel, since it
      // may continue to be used after function exit.  So we allocate this on
      // top.
      if (bigstack_end_alloc_ui(calc_thread_ct + 1, &thread_start)) {
        goto calc_grm_ret_NOMEM;
      }
      // slightly different from plink 1.9 since we don't bother to treat the
      // diagonal as a special case any more.
      triangle_fill(sample_ct, calc_thread_ct, parallel_idx, parallel_tot, 0, 1, thread_start);
      row_start_idx = thread_start[0];
      row_end_idx = thread_start[calc_thread_ct];
      if (row_end_idx < sample_ct) {
        // 0
        // 0 0
        // 0 0 0
        // 0 0 0 0
        // 1 1 1 1 1
        // 1 1 1 1 1 1
        // 2 2 2 2 2 2 2
        // 2 2 2 2 2 2 2 2
        // If we're computing part 0, we never need to load the last 4 samples;
        // if part 1, we don't need the last two; etc.
        uintptr_t* new_sample_include;
        if (bigstack_alloc_ul(raw_sample_ctl, &new_sample_include)) {
          goto calc_grm_ret_NOMEM;
        }
        const uint32_t sample_uidx_end = 1 + idx_to_uidx_basic(orig_sample_include, row_end_idx - 1);
        memcpy(new_sample_include, orig_sample_include, round_up_pow2(sample_uidx_end, kBitsPerWord) / CHAR_BIT);
        clear_bits_nz(sample_uidx_end, raw_sample_ctl * kBitsPerWord, new_sample_include);
        sample_include = new_sample_include;
      }
    }
    g_thread_start = thread_start;
    double* grm;
    if (bigstack_calloc_d((row_end_idx - row_start_idx) * row_end_idx, &grm)) {
      goto calc_grm_ret_NOMEM;
    }
    g_pca_sample_ct = row_end_idx;
    g_grm = grm;
    const uint32_t row_end_idxl2 = QUATERCT_TO_WORDCT(row_end_idx);
    const uint32_t row_end_idxl = BITCT_TO_WORDCT(row_end_idx);
    uint32_t* sample_include_cumulative_popcounts;
    uintptr_t* genovec_buf;
    uintptr_t* dosage_present_buf;
    dosage_t* dosage_vals_buf;
    if (bigstack_alloc_ui(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_thread(calc_thread_ct, &ts.threads) ||
        bigstack_alloc_ul(row_end_idxl2, &genovec_buf) ||
        bigstack_alloc_ul(row_end_idxl, &dosage_present_buf) ||
        bigstack_alloc_dosage(row_end_idx, &dosage_vals_buf)) {
      goto calc_grm_ret_NOMEM;
    }
    fill_cumulative_popcounts(sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
    reterr = conditional_allocate_non_autosomal_variants(cip, "GRM construction", raw_variant_ct, &variant_include, &variant_ct);
    if (reterr) {
      goto calc_grm_ret_1;
    }
    if (bigstack_alloc_d(row_end_idx * kGrmVariantBlockSize, &g_normed_dosage_vmaj_bufs[0]) ||
        bigstack_alloc_d(row_end_idx * kGrmVariantBlockSize, &g_normed_dosage_vmaj_bufs[1])) {
      goto calc_grm_ret_NOMEM;
    }
    const uint32_t raw_variant_ctl = BITCT_TO_WORDCT(raw_variant_ct);
    uintptr_t* variant_include_has_missing = nullptr;
    if (!(grm_flags & kfGrmMeanimpute)) {
      if (bigstack_calloc_ul(raw_variant_ctl, &variant_include_has_missing)) {
        goto calc_grm_ret_NOMEM;
      }
    }
    if (thread_start) {
      if (bigstack_alloc_d(row_end_idx * kGrmVariantBlockSize, &g_normed_dosage_smaj_bufs[0]) ||
          bigstack_alloc_d(row_end_idx * kGrmVariantBlockSize, &g_normed_dosage_smaj_bufs[1])) {
        goto calc_grm_ret_NOMEM;
      }
    }
#ifdef USE_MTBLAS
    const uint32_t blas_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    BLAS_SET_NUM_THREADS(blas_thread_ct);
#endif
    // Main workflow:
    // 1. Set n=0, load batch 0
    //
    // 2. Spawn threads processing batch n
    // 3. Increment n by 1
    // 4. Load batch n unless eof
    // 5. Join threads
    // 6. Goto step 2 unless eof
    const uint32_t variance_standardize = !(grm_flags & kfGrmCov);
    uint32_t parity = 0;
    uint32_t cur_variant_idx_start = 0;
    uint32_t variant_uidx = 0;
    uint32_t cur_allele_ct = 2;
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    logprint("Constructing GRM: ");
    fputs("0%", stdout);
    fflush(stdout);
    pgr_clear_ld_cache(simple_pgrp);
    while (1) {
      uint32_t cur_batch_size = 0;
      if (!ts.is_last_block) {
        cur_batch_size = kGrmVariantBlockSize;
        uint32_t cur_variant_idx_end = cur_variant_idx_start + cur_batch_size;
        if (cur_variant_idx_end > variant_ct) {
          cur_batch_size = variant_ct - cur_variant_idx_start;
          cur_variant_idx_end = variant_ct;
        }
        double* normed_vmaj_iter = g_normed_dosage_vmaj_bufs[parity];
        for (uint32_t variant_idx = cur_variant_idx_start; variant_idx < cur_variant_idx_end; ++variant_uidx, ++variant_idx) {
          next_set_unsafe_ck(variant_include, &variant_uidx);
          const uint32_t maj_allele_idx = maj_alleles[variant_uidx];
          uint32_t missing_present = 0;
          uintptr_t allele_idx_base;
          if (!variant_allele_idxs) {
            allele_idx_base = variant_uidx;
          } else {
            allele_idx_base = variant_allele_idxs[variant_uidx];
            cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - allele_idx_base;
            allele_idx_base -= variant_uidx;
          }
          reterr = load_centered_varmaj(sample_include, sample_include_cumulative_popcounts, variance_standardize, row_end_idx, variant_uidx, maj_allele_idx, get_allele_freq(&(allele_freqs[allele_idx_base]), maj_allele_idx, cur_allele_ct), simple_pgrp, variant_include_has_missing? (&missing_present) : nullptr, normed_vmaj_iter, genovec_buf, dosage_present_buf, dosage_vals_buf);
          if (reterr) {
            if (reterr == kPglRetInconsistentInput) {
              logprint("\n");
              logerrprint("Error: Zero-MAF variant is not actually monomorphic.  (This is possible when\ne.g. MAF is estimated from founders, but the minor allele was only observed in\nnonfounders.  In any case, you should be using e.g. --maf to filter out all\nvery-low-MAF variants, since the relationship matrix distance formula does not\nhandle them well.)\n");
            } else if (reterr == kPglRetMalformedInput) {
              logprint("\n");
              logerrprint("Error: Malformed .pgen file.\n");
            }
            goto calc_grm_ret_1;
          }
          if (missing_present) {
            set_bit(variant_uidx, variant_include_has_missing);
          }
          normed_vmaj_iter = &(normed_vmaj_iter[row_end_idx]);
        }
        if (thread_start) {
          transpose_copy(g_normed_dosage_vmaj_bufs[parity], cur_batch_size, row_end_idx, g_normed_dosage_smaj_bufs[parity]);
        }
      }
      if (cur_variant_idx_start) {
        join_threads3z(&ts);
        if (ts.is_last_block) {
          break;
        }
        if (cur_variant_idx_start >= next_print_variant_idx) {
          if (pct > 10) {
            putc_unlocked('\b', stdout);
          }
          pct = (cur_variant_idx_start * 100LLU) / variant_ct;
          printf("\b\b%u%%", pct++);
          fflush(stdout);
          next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
        }
      }
      ts.is_last_block = (cur_variant_idx_start + cur_batch_size == variant_ct);
      g_cur_batch_size = cur_batch_size;
      if (!ts.thread_func_ptr) {
        if (thread_start) {
          ts.thread_func_ptr = calc_grm_part_thread;
        } else {
          ts.thread_func_ptr = calc_grm_thread;
        }
      }
      if (spawn_threads3z(cur_variant_idx_start, &ts)) {
        goto calc_grm_ret_THREAD_CREATE_FAIL;
      }
      cur_variant_idx_start += cur_batch_size;
      parity = 1 - parity;
    }
    BLAS_SET_NUM_THREADS(1);
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprint("done.\n");
    uint32_t* missing_cts = nullptr; // stays null iff meanimpute
    uint32_t* missing_dbl_exclude_cts = nullptr;
    if (variant_include_has_missing) {
      const uint32_t variant_ct_with_missing = popcount_longs(variant_include_has_missing, raw_variant_ctl);
      // if no missing calls at all, act as if meanimpute was on
      if (variant_ct_with_missing) {
        logprint("Correcting for missingness... ");
        reterr = calc_missing_matrix(sample_include, sample_include_cumulative_popcounts, variant_include_has_missing, sample_ct, variant_ct_with_missing, parallel_idx, parallel_tot, row_start_idx, row_end_idx, max_thread_ct, simple_pgrp, &missing_cts, &missing_dbl_exclude_cts);
        if (reterr) {
          goto calc_grm_ret_1;
        }
      }
    }
    if (missing_cts) {
      // could parallelize this loop if it ever matters
      const uint32_t* missing_dbl_exclude_iter = missing_dbl_exclude_cts;
      for (uintptr_t row_idx = row_start_idx; row_idx < row_end_idx; ++row_idx) {
        const uint32_t variant_ct_base = variant_ct - missing_cts[row_idx];
        double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
        for (uint32_t col_idx = 0; col_idx < row_idx; ++col_idx) {
          *grm_iter++ /= u31tod(variant_ct_base - missing_cts[col_idx] + (*missing_dbl_exclude_iter++));
        }
        *grm_iter++ /= u31tod(variant_ct_base);
      }
    } else {
      const double variant_ct_recip = 1.0 / u31tod(variant_ct);
      for (uintptr_t row_idx = row_start_idx; row_idx < row_end_idx; ++row_idx) {
        double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
        for (uint32_t col_idx = 0; col_idx <= row_idx; ++col_idx) {
          *grm_iter++ *= variant_ct_recip;
        }
      }
    }
    // N.B. Only the lower right of grm[] is valid when parallel_tot == 1.

    // possible todo: allow simultaneous --make-rel and
    // --make-grm-list/--make-grm-bin
    // (note that this routine may also be called by --pca, which may not write
    // a matrix to disk at all.)
    if (grm_flags & (kfGrmMatrixShapemask | kfGrmListmask | kfGrmBin)) {
      const grm_flags_t matrix_shape = grm_flags & kfGrmMatrixShapemask;
      char* log_write_iter;
      if (matrix_shape) {
        // --make-rel
        fputs("--make-rel: Writing...", stdout);
        fflush(stdout);
        if (grm_flags & kfGrmMatrixBin) {
          char* outname_end2 = strcpya(outname_end, ".rel.bin");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          *outname_end2 = '\0';
          if (fopen_checked(outname, FOPEN_WB, &outfile)) {
            goto calc_grm_ret_OPEN_FAIL;
          }
          double* write_double_buf = nullptr;
          if (matrix_shape == kfGrmMatrixSq0) {
            write_double_buf = R_CAST(double*, g_textbuf);
            fill_double_zero(kTextbufMainSize / sizeof(double), write_double_buf);
          } else if (matrix_shape == kfGrmMatrixSq) {
            if (bigstack_alloc_d(row_end_idx - row_start_idx - 1, &write_double_buf)) {
              goto calc_grm_ret_NOMEM;
            }
          }
          uintptr_t row_idx = row_start_idx;
          while (1) {
            const double* grm_row = &(grm[(row_idx - row_start_idx) * row_end_idx]);
            ++row_idx;
            if (fwrite_checked(grm_row, row_idx * sizeof(double), outfile)) {
              goto calc_grm_ret_WRITE_FAIL;
            }
            if (row_idx == row_end_idx) {
              break;
            }
            if (matrix_shape == kfGrmMatrixSq0) {
              uintptr_t zbytes_to_dump = (sample_ct - row_idx) * sizeof(double);
              while (zbytes_to_dump >= kTextbufMainSize) {
                if (fwrite_checked(write_double_buf, kTextbufMainSize, outfile)) {
                  goto calc_grm_ret_WRITE_FAIL;
                }
                zbytes_to_dump -= kTextbufMainSize;
              }
              if (zbytes_to_dump) {
                if (fwrite_checked(write_double_buf, zbytes_to_dump, outfile)) {
                  goto calc_grm_ret_WRITE_FAIL;
                }
              }
            } else if (matrix_shape == kfGrmMatrixSq) {
              double* write_double_iter = write_double_buf;
              const double* grm_col = &(grm[row_idx - 1]);
              for (uintptr_t row_idx2 = row_idx; row_idx2 < sample_ct; ++row_idx2) {
                *write_double_iter++ = grm_col[(row_idx2 - row_start_idx) * sample_ct];
              }
              if (fwrite_checked(write_double_buf, (sample_ct - row_idx) * sizeof(double), outfile)) {
                goto calc_grm_ret_WRITE_FAIL;
              }
            }
          }
          if (fclose_null(&outfile)) {
            goto calc_grm_ret_WRITE_FAIL;
          }
        } else if (grm_flags & kfGrmMatrixBin4) {
          // downcode all entries to floats
          char* outname_end2 = strcpya(outname_end, ".rel.bin");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          *outname_end2 = '\0';
          if (fopen_checked(outname, FOPEN_WB, &outfile)) {
            goto calc_grm_ret_OPEN_FAIL;
          }
          float* write_float_buf;
          if (bigstack_alloc_f(row_end_idx, &write_float_buf)) {
            goto calc_grm_ret_NOMEM;
          }
          uintptr_t row_idx = row_start_idx;
          do {
            const double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
            float* write_float_iter = write_float_buf;
            for (uint32_t col_idx = 0; col_idx <= row_idx; ++col_idx) {
              *write_float_iter++ = S_CAST(float, *grm_iter++);
            }
            ++row_idx;
            if (matrix_shape == kfGrmMatrixSq0) {
              fill_float_zero(sample_ct - row_idx, write_float_iter);
              write_float_iter = &(write_float_buf[sample_ct]);
            } else if (matrix_shape == kfGrmMatrixSq) {
              const double* grm_col = &(grm[row_idx - 1]);
              for (uintptr_t row_idx2 = row_idx; row_idx2 < sample_ct; ++row_idx2) {
                *write_float_iter++ = S_CAST(float, grm_col[(row_idx2 - row_start_idx) * sample_ct]);
              }
            }
            if (fwrite_checked(write_float_buf, sizeof(float) * S_CAST(uintptr_t, write_float_iter - write_float_buf), outfile)) {
              goto calc_grm_ret_WRITE_FAIL;
            }
          } while (row_idx < row_end_idx);
          if (fclose_null(&outfile)) {
            goto calc_grm_ret_WRITE_FAIL;
          }
        } else {
          char* outname_end2 = strcpya(outname_end, ".rel");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          const uint32_t output_zst = (grm_flags / kfGrmMatrixZs) & 1;
          if (output_zst) {
            outname_end2 = strcpya(outname_end2, ".zst");
          }
          *outname_end2 = '\0';
          reterr = cswrite_init2(outname, 0, output_zst, max_thread_ct, kCompressStreamBlock + 16 * row_end_idx, &css, &cswritep);
          if (reterr) {
            goto calc_grm_ret_1;
          }
          uintptr_t row_idx = row_start_idx;
          do {
            const double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
            ++row_idx;
            for (uint32_t col_idx = 0; col_idx < row_idx; ++col_idx) {
              cswritep = dtoa_g(*grm_iter++, cswritep);
              *cswritep++ = '\t';
            }
            if (matrix_shape == kfGrmMatrixSq0) {
              // (roughly same performance as creating a zero-tab constant
              // buffer in advance)
              const uint32_t zcount = sample_ct - row_idx;
              const uint32_t wct = DIV_UP(zcount, kBytesPerWord / 2);
              // assumes little-endian
              const uintptr_t zerotab_word = 0x930 * kMask0001;
#ifdef __arm__
#  error "Unaligned accesses in calc_grm()."
#endif
              uintptr_t* writep_alias = R_CAST(uintptr_t*, cswritep);
              for (uintptr_t widx = 0; widx < wct; ++widx) {
                *writep_alias++ = zerotab_word;
              }
              cswritep = &(cswritep[zcount * 2]);
            } else if (matrix_shape == kfGrmMatrixSq) {
              const double* grm_col = &(grm[row_idx - 1]);
              for (uintptr_t row_idx2 = row_idx; row_idx2 < sample_ct; ++row_idx2) {
                cswritep = dtoa_g(grm_col[(row_idx2 - row_start_idx) * sample_ct], cswritep);
                *cswritep++ = '\t';
              }
            }
            decr_append_binary_eoln(&cswritep);
            if (cswrite(&css, &cswritep)) {
              goto calc_grm_ret_WRITE_FAIL;
            }
          } while (row_idx < row_end_idx);
          if (cswrite_close_null(&css, cswritep)) {
            goto calc_grm_ret_WRITE_FAIL;
          }
        }
        putc_unlocked('\r', stdout);
        log_write_iter = strcpya(g_logbuf, "--make-rel: GRM ");
        if (parallel_tot != 1) {
          log_write_iter = strcpya(log_write_iter, "component ");
        }
        log_write_iter = strcpya(log_write_iter, "written to ");
        log_write_iter = strcpya(log_write_iter, outname);
      } else {
        const uint32_t* missing_dbl_exclude_iter = missing_dbl_exclude_cts;
        if (grm_flags & kfGrmBin) {
          // --make-grm-bin
          float* write_float_buf;
          if (bigstack_alloc_f(row_end_idx, &write_float_buf)) {
            goto calc_grm_ret_NOMEM;
          }
          char* outname_end2 = strcpya(outname_end, ".grm.bin");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          *outname_end2 = '\0';
          if (fopen_checked(outname, FOPEN_WB, &outfile)) {
            goto calc_grm_ret_OPEN_FAIL;
          }
          fputs("--make-grm-bin: Writing...", stdout);
          fflush(stdout);
          for (uintptr_t row_idx = row_start_idx; row_idx < row_end_idx; ++row_idx) {
            const double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
            for (uint32_t col_idx = 0; col_idx <= row_idx; ++col_idx) {
              write_float_buf[col_idx] = S_CAST(float, *grm_iter++);
            }
            if (fwrite_checked(write_float_buf, (row_idx + 1) * sizeof(float), outfile)) {
              goto calc_grm_ret_WRITE_FAIL;
            }
          }
          if (fclose_null(&outfile)) {
            goto calc_grm_ret_WRITE_FAIL;
          }

          outname_end2 = strcpya(outname_end, ".grm.N.bin");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          *outname_end2 = '\0';
          if (fopen_checked(outname, FOPEN_WB, &outfile)) {
            goto calc_grm_ret_OPEN_FAIL;
          }
          if (!missing_cts) {
            // trivial case: write the same number repeatedly
            const uintptr_t tot_cells = (S_CAST(uint64_t, row_end_idx) * (row_end_idx - 1) - S_CAST(uint64_t, row_start_idx) * (row_start_idx - 1)) / 2;
            const float variant_ctf = u31tof(variant_ct);
            write_float_buf = R_CAST(float*, g_textbuf);
            for (uint32_t uii = 0; uii < (kTextbufMainSize / sizeof(float)); ++uii) {
              write_float_buf[uii] = variant_ctf;
            }
            const uintptr_t full_write_ct = tot_cells / (kTextbufMainSize / sizeof(float));
            for (uintptr_t ulii = 0; ulii < full_write_ct; ++ulii) {
              if (fwrite_checked(write_float_buf, kTextbufMainSize, outfile)) {
                goto calc_grm_ret_WRITE_FAIL;
              }
            }
            const uintptr_t remainder = tot_cells % (kTextbufMainSize / sizeof(float));
            if (remainder) {
              if (fwrite_checked(write_float_buf, remainder * sizeof(float), outfile)) {
                goto calc_grm_ret_WRITE_FAIL;
              }
            }
          } else {
            for (uintptr_t row_idx = row_start_idx; row_idx < row_end_idx; ++row_idx) {
              const uint32_t variant_ct_base = variant_ct - missing_cts[row_idx];
              for (uint32_t col_idx = 0; col_idx <= row_idx; ++col_idx) {
                uint32_t cur_obs_ct = variant_ct_base;
                if (col_idx != row_idx) {
                  cur_obs_ct = cur_obs_ct - missing_cts[col_idx] + (*missing_dbl_exclude_iter++);
                }
                write_float_buf[col_idx] = u31tof(cur_obs_ct);
              }
              if (fwrite_checked(write_float_buf, (row_idx + 1) * sizeof(float), outfile)) {
                goto calc_grm_ret_WRITE_FAIL;
              }
            }
          }
          if (fclose_null(&outfile)) {
            goto calc_grm_ret_WRITE_FAIL;
          }
          putc_unlocked('\r', stdout);
          const uint32_t outname_copy_byte_ct = 5 + S_CAST(uintptr_t, outname_end - outname);
          log_write_iter = strcpya(g_logbuf, "--make-grm-bin: GRM ");
          if (parallel_tot != 1) {
            log_write_iter = strcpya(log_write_iter, "component ");
          }
          log_write_iter = strcpya(log_write_iter, "written to ");
          log_write_iter = memcpya(log_write_iter, outname, outname_copy_byte_ct);
          log_write_iter = memcpyl3a(log_write_iter, "bin");
          if (parallel_tot != 1) {
            *log_write_iter++ = '.';
            log_write_iter = uint32toa(parallel_idx + 1, log_write_iter);
          }
          log_write_iter = memcpyl3a(log_write_iter, " , ");
          if (parallel_idx) {
            log_write_iter = strcpya(log_write_iter, "and ");
          }
          log_write_iter = strcpya(log_write_iter, "observation counts to ");
          log_write_iter = memcpya(log_write_iter, outname, outname_end2 - outname);
        } else {
          // --make-grm-list
          char* outname_end2 = strcpya(outname_end, ".grm");
          if (parallel_tot != 1) {
            *outname_end2++ = '.';
            outname_end2 = uint32toa(parallel_idx + 1, outname_end2);
          }
          if (grm_flags & kfGrmListZs) {
            outname_end2 = strcpya(outname_end2, ".zst");
          }
          *outname_end2 = '\0';
          reterr = cswrite_init2(outname, 0, !(grm_flags & kfGrmListNoGz), max_thread_ct, kCompressStreamBlock + kMaxMediumLine, &css, &cswritep);
          if (reterr) {
            goto calc_grm_ret_1;
          }
          fputs("--make-grm-list: Writing...", stdout);
          fflush(stdout);
          for (uintptr_t row_idx = row_start_idx; row_idx < row_end_idx; ++row_idx) {
            uint32_t variant_ct_base = variant_ct;
            if (missing_cts) {
              variant_ct_base -= missing_cts[row_idx];
            }
            const double* grm_iter = &(grm[(row_idx - row_start_idx) * row_end_idx]);
            for (uint32_t col_idx = 0; col_idx <= row_idx; ++col_idx) {
              cswritep = uint32toa_x(row_idx + 1, '\t', cswritep);
              cswritep = uint32toa_x(col_idx + 1, '\t', cswritep);
              if (missing_cts) {
                uint32_t cur_obs_ct = variant_ct_base;
                if (col_idx != row_idx) {
                  cur_obs_ct = cur_obs_ct - missing_cts[col_idx] + (*missing_dbl_exclude_iter++);
                }
                cswritep = uint32toa(cur_obs_ct, cswritep);
              } else {
                cswritep = uint32toa(variant_ct_base, cswritep);
              }
              *cswritep++ = '\t';
              cswritep = dtoa_g(*grm_iter++, cswritep);
              append_binary_eoln(&cswritep);
              if (cswrite(&css, &cswritep)) {
                goto calc_grm_ret_WRITE_FAIL;
              }
            }
          }
          if (cswrite_close_null(&css, cswritep)) {
            goto calc_grm_ret_WRITE_FAIL;
          }
          putc_unlocked('\r', stdout);
          log_write_iter = strcpya(g_logbuf, "--make-grm-list: GRM ");
          if (parallel_tot != 1) {
            log_write_iter = strcpya(log_write_iter, "component ");
          }
          log_write_iter = strcpya(log_write_iter, "written to ");
          log_write_iter = strcpya(log_write_iter, outname);
        }
      }
      if (!parallel_idx) {
        snprintf(&(outname_end[4]), kMaxOutfnameExtBlen - 4, ".id");
        reterr = write_sample_ids(orig_sample_include, sample_ids, sids, outname, sample_ct, max_sample_id_blen, max_sid_blen, (grm_flags / kfGrmNoIdheader) & 1);
        if (reterr) {
          goto calc_grm_ret_1;
        }
        log_write_iter = strcpya(log_write_iter, " , and IDs to ");
        log_write_iter = strcpya(log_write_iter, outname);
      }
      snprintf(log_write_iter, kLogbufSize - 2 * kPglFnamesize - 256, " .\n");
      wordwrapb(0);
      logprintb();
    }

    if (grm_ptr) {
      *grm_ptr = grm;
      // allocation right on top of grm[]
      bigstack_mark = R_CAST(unsigned char*, sample_include_cumulative_popcounts);
    }
  }
  while (0) {
  calc_grm_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  calc_grm_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  calc_grm_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  calc_grm_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  calc_grm_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 calc_grm_ret_1:
  cswrite_close_cond(&css, cswritep);
  ZWRAP_useZSTDcompression(1);
  fclose_cond(outfile);
  threads3z_cleanup(&ts, &g_cur_batch_size);
  BLAS_SET_NUM_THREADS(1);
  bigstack_double_reset(bigstack_mark, bigstack_end_mark);
  return reterr;
}

// should be able to remove NOLAPACK later since we already have a non-LAPACK
// SVD implementation
#ifndef NOLAPACK
// this seems to be better than 256 (due to avoidance of cache critical
// stride?)
// (still want this to be a multiple of 8, for cleaner multithreading)
CONSTU31(kPcaVariantBlockSize, 240);

// multithread globals
static uintptr_t* g_genovecs[2] = {nullptr, nullptr};
static uint32_t* g_dosage_cts[2] = {nullptr, nullptr};
static uintptr_t* g_dosage_presents[2] = {nullptr, nullptr};
static dosage_t* g_dosage_val_bufs[2] = {nullptr, nullptr};
static double* g_cur_maj_freqs[2] = {nullptr, nullptr};
static double** g_yy_bufs = nullptr;
static double** g_y_transpose_bufs = nullptr;
static double** g_g2_bb_part_bufs = nullptr;
static double* g_g1 = nullptr;
static double* g_qq = nullptr;

static uint32_t g_pc_ct = 0;
static pglerr_t g_error_ret = kPglRetSuccess;

THREAD_FUNC_DECL calc_pca_xtxa_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t pca_sample_ct = g_pca_sample_ct;
  const uintptr_t pca_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uintptr_t pca_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uint32_t pc_ct_x2 = g_pc_ct * 2;
  const uintptr_t qq_col_ct = (g_pc_ct + 1) * pc_ct_x2;
  const uint32_t vidx_offset = tidx * kPcaVariantBlockSize;
  const double* g1 = g_g1;
  double* qq_iter = g_qq;
  double* yy_buf = g_yy_bufs[tidx];
  double* y_transpose_buf = g_y_transpose_bufs[tidx];
  double* g2_part_buf = g_g2_bb_part_bufs[tidx];
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (vidx_offset < cur_batch_size) {
      uint32_t cur_thread_batch_size = cur_batch_size - vidx_offset;
      if (cur_thread_batch_size > kPcaVariantBlockSize) {
        cur_thread_batch_size = kPcaVariantBlockSize;
      }
      const uintptr_t* genovec_iter = &(g_genovecs[parity][vidx_offset * pca_sample_ctaw2]);
      const uint32_t* cur_dosage_cts = &(g_dosage_cts[parity][vidx_offset]);
      const uintptr_t* dosage_present_iter = &(g_dosage_presents[parity][vidx_offset * pca_sample_ctaw]);
      const dosage_t* dosage_vals_iter = &(g_dosage_val_bufs[parity][vidx_offset * pca_sample_ct]);
      const double* cur_maj_freqs_iter = &(g_cur_maj_freqs[parity][vidx_offset]);
      double* yy_iter = yy_buf;
      for (uint32_t uii = 0; uii < cur_thread_batch_size; ++uii) {
        pglerr_t reterr = expand_centered_varmaj(genovec_iter, dosage_present_iter, dosage_vals_iter, 1, pca_sample_ct, cur_dosage_cts[uii], cur_maj_freqs_iter[uii], yy_iter);
        if (reterr) {
          g_error_ret = reterr;
          break;
        }
        yy_iter = &(yy_iter[pca_sample_ct]);
        genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
        dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
        dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
      }
      double* cur_qq = &(qq_iter[vidx_offset * qq_col_ct]);
      row_major_matrix_multiply_strided(yy_buf, g1, cur_thread_batch_size, pca_sample_ct, pc_ct_x2, pc_ct_x2, pca_sample_ct, qq_col_ct, cur_qq);
      transpose_copy(yy_buf, cur_thread_batch_size, pca_sample_ct, y_transpose_buf);
      row_major_matrix_multiply_strided_incr(y_transpose_buf, cur_qq, pca_sample_ct, cur_thread_batch_size, pc_ct_x2, qq_col_ct, cur_thread_batch_size, pc_ct_x2, g2_part_buf);
      qq_iter = &(qq_iter[cur_batch_size * qq_col_ct]);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

THREAD_FUNC_DECL calc_pca_xa_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t pca_sample_ct = g_pca_sample_ct;
  const uintptr_t pca_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uintptr_t pca_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uint32_t pc_ct_x2 = g_pc_ct * 2;
  const uintptr_t qq_col_ct = (g_pc_ct + 1) * pc_ct_x2;
  const uint32_t vidx_offset = tidx * kPcaVariantBlockSize;
  const double* g1 = g_g1;
  double* qq_iter = g_qq;
  double* yy_buf = g_yy_bufs[tidx];
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (vidx_offset < cur_batch_size) {
      uint32_t cur_thread_batch_size = cur_batch_size - vidx_offset;
      if (cur_thread_batch_size > kPcaVariantBlockSize) {
        cur_thread_batch_size = kPcaVariantBlockSize;
      }
      const uintptr_t* genovec_iter = &(g_genovecs[parity][vidx_offset * pca_sample_ctaw2]);
      const uint32_t* cur_dosage_cts = &(g_dosage_cts[parity][vidx_offset]);
      const uintptr_t* dosage_present_iter = &(g_dosage_presents[parity][vidx_offset * pca_sample_ctaw]);
      const dosage_t* dosage_vals_iter = &(g_dosage_val_bufs[parity][vidx_offset * pca_sample_ct]);
      const double* cur_maj_freqs_iter = &(g_cur_maj_freqs[parity][vidx_offset]);
      double* yy_iter = yy_buf;
      for (uint32_t uii = 0; uii < cur_thread_batch_size; ++uii) {
        pglerr_t reterr = expand_centered_varmaj(genovec_iter, dosage_present_iter, dosage_vals_iter, 1, pca_sample_ct, cur_dosage_cts[uii], cur_maj_freqs_iter[uii], yy_iter);
        if (reterr) {
          g_error_ret = reterr;
          break;
        }
        yy_iter = &(yy_iter[pca_sample_ct]);
        genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
        dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
        dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
      }
      double* cur_qq = &(qq_iter[vidx_offset * qq_col_ct]);
      row_major_matrix_multiply_strided(yy_buf, g1, cur_thread_batch_size, pca_sample_ct, pc_ct_x2, pc_ct_x2, pca_sample_ct, qq_col_ct, cur_qq);
      qq_iter = &(qq_iter[cur_batch_size * qq_col_ct]);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

THREAD_FUNC_DECL calc_pca_xtb_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t pca_sample_ct = g_pca_sample_ct;
  const uintptr_t pca_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uintptr_t pca_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uint32_t pc_ct_x2 = g_pc_ct * 2;
  const uintptr_t qq_col_ct = (g_pc_ct + 1) * pc_ct_x2;
  const uint32_t vidx_offset = tidx * kPcaVariantBlockSize;
  const double* qq_iter = &(g_qq[vidx_offset * qq_col_ct]);
  double* yy_buf = g_yy_bufs[tidx];
  double* y_transpose_buf = g_y_transpose_bufs[tidx];
  double* bb_part_buf = g_g2_bb_part_bufs[tidx];
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (vidx_offset < cur_batch_size) {
      uint32_t cur_thread_batch_size = cur_batch_size - vidx_offset;
      if (cur_thread_batch_size > kPcaVariantBlockSize) {
        cur_thread_batch_size = kPcaVariantBlockSize;
      }
      const uintptr_t* genovec_iter = &(g_genovecs[parity][vidx_offset * pca_sample_ctaw2]);
      const uint32_t* cur_dosage_cts = &(g_dosage_cts[parity][vidx_offset]);
      const uintptr_t* dosage_present_iter = &(g_dosage_presents[parity][vidx_offset * pca_sample_ctaw]);
      const dosage_t* dosage_vals_iter = &(g_dosage_val_bufs[parity][vidx_offset * pca_sample_ct]);
      const double* cur_maj_freqs_iter = &(g_cur_maj_freqs[parity][vidx_offset]);
      double* yy_iter = yy_buf;
      for (uint32_t uii = 0; uii < cur_thread_batch_size; ++uii) {
        pglerr_t reterr = expand_centered_varmaj(genovec_iter, dosage_present_iter, dosage_vals_iter, 1, pca_sample_ct, cur_dosage_cts[uii], cur_maj_freqs_iter[uii], yy_iter);
        if (reterr) {
          g_error_ret = reterr;
          break;
        }
        yy_iter = &(yy_iter[pca_sample_ct]);
        genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
        dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
        dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
      }
      transpose_copy(yy_buf, cur_thread_batch_size, pca_sample_ct, y_transpose_buf);
      row_major_matrix_multiply_incr(y_transpose_buf, qq_iter, pca_sample_ct, qq_col_ct, cur_thread_batch_size, bb_part_buf);
      qq_iter = &(qq_iter[cur_batch_size * qq_col_ct]);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

THREAD_FUNC_DECL calc_pca_var_wts_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t pca_sample_ct = g_pca_sample_ct;
  const uintptr_t pca_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uintptr_t pca_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(pca_sample_ct);
  const uint32_t pc_ct = g_pc_ct;
  const uint32_t vidx_offset = tidx * kPcaVariantBlockSize;

  // either first batch size is calc_thread_ct * kPcaVariantBlockSize, or there
  // is only one batch
  const uintptr_t var_wts_part_size = S_CAST(uintptr_t, pc_ct) * g_cur_batch_size;

  const double* sample_wts = g_g1; // sample-major, pc_ct columns
  double* yy_buf = g_yy_bufs[tidx];
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (vidx_offset < cur_batch_size) {
      uint32_t cur_thread_batch_size = cur_batch_size - vidx_offset;
      if (cur_thread_batch_size > kPcaVariantBlockSize) {
        cur_thread_batch_size = kPcaVariantBlockSize;
      }
      const uintptr_t* genovec_iter = &(g_genovecs[parity][vidx_offset * pca_sample_ctaw2]);
      const uint32_t* cur_dosage_cts = &(g_dosage_cts[parity][vidx_offset]);
      const uintptr_t* dosage_present_iter = &(g_dosage_presents[parity][vidx_offset * pca_sample_ctaw]);
      const dosage_t* dosage_vals_iter = &(g_dosage_val_bufs[parity][vidx_offset * pca_sample_ct]);
      const double* cur_maj_freqs_iter = &(g_cur_maj_freqs[parity][vidx_offset]);
      double* yy_iter = yy_buf;
      for (uint32_t uii = 0; uii < cur_thread_batch_size; ++uii) {
        pglerr_t reterr = expand_centered_varmaj(genovec_iter, dosage_present_iter, dosage_vals_iter, 1, pca_sample_ct, cur_dosage_cts[uii], cur_maj_freqs_iter[uii], yy_iter);
        if (reterr) {
          g_error_ret = reterr;
          break;
        }
        yy_iter = &(yy_iter[pca_sample_ct]);
        genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
        dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
        dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
      }
      // Variant weight matrix = X^T * S * D^{-1/2}, where X^T is the
      // variance-standardized genotype matrix, S is the sample weight matrix,
      // and D is a diagonal eigenvalue matrix.
      // We postpone the D^{-1/2} part for now, but it's straightforward to
      // switch to using precomputed (S * D^{-1/2}).
      double* cur_var_wts_part = &(g_qq[parity * var_wts_part_size + vidx_offset * S_CAST(uintptr_t, pc_ct)]);
      row_major_matrix_multiply(yy_buf, sample_wts, cur_thread_batch_size, pc_ct, pca_sample_ct, cur_var_wts_part);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

pglerr_t calc_pca(const uintptr_t* sample_include, const char* sample_ids, const char* sids, uintptr_t* variant_include, const chr_info_t* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const alt_allele_ct_t* maj_alleles, const double* allele_freqs, uint32_t raw_sample_ct, uintptr_t pca_sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t pc_ct, pca_flags_t pca_flags, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, double* grm, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  char* cswritep = nullptr;
  compress_stream_state_t css;
  threads_state_t ts;
  init_threads3z(&ts);
  pglerr_t reterr = kPglRetSuccess;
  cswrite_init_null(&css);
  {
    if ((pca_flags & kfPcaSid) && (!sids)) {
      // put this in plink2_common?
      const uint32_t dummy_sids_word_ct = DIV_UP(raw_sample_ct, (kBytesPerWord / 2));
      uintptr_t* dummy_sids;
      if (bigstack_alloc_ul(dummy_sids_word_ct, &dummy_sids)) {
        goto calc_pca_ret_NOMEM;
      }
      // repeated "0\0", little-endian
      const uintptr_t text_zero_word = kMask0001 * 48;
      for (uint32_t uii = 0; uii < dummy_sids_word_ct; ++uii) {
        dummy_sids[uii] = text_zero_word;
      }
      sids = R_CAST(char*, dummy_sids);
      max_sid_blen = 2;
    }
    const uint32_t is_approx = (pca_flags / kfPcaApprox) & 1;
    reterr = conditional_allocate_non_autosomal_variants(cip, is_approx? "PCA approximation" : "PCA", raw_variant_ct, &variant_include, &variant_ct);
    if (reterr) {
      goto calc_pca_ret_1;
    }
#ifdef __APPLE__
    // min OS X version is 10.7, so we can take Grand Central Dispatch dgemm
    // for granted
    // (tried this with Linux MKL + OpenMP as well, but results were inferior)
    uint32_t calc_thread_ct = 1;
#else
    // I/O thread generally has <1/8 of workload
    uint32_t calc_thread_ct = (max_thread_ct > 8)? (max_thread_ct - 1) : max_thread_ct;
    if ((calc_thread_ct - 1) * kPcaVariantBlockSize >= variant_ct) {
      calc_thread_ct = 1 + (variant_ct - 1) / kPcaVariantBlockSize;
    }
#endif
    ts.calc_thread_ct = calc_thread_ct;
    if (pc_ct > pca_sample_ct) {
      if (pca_sample_ct <= variant_ct) {
        pc_ct = pca_sample_ct;
        snprintf(g_logbuf, kLogbufSize, "Warning: calculating %u PCs, since there are only %u samples.\n", pc_ct, pc_ct);
      } else {
        pc_ct = variant_ct;
        snprintf(g_logbuf, kLogbufSize, "Warning: calculating %u PCs, since there are only %u autosomal variants.\n", pc_ct, pc_ct);
      }
      if (pc_ct < 2) {
        logerrprint("Error: Too few samples or autosomal variants for PCA.\n");
        goto calc_pca_ret_INCONSISTENT_INPUT;
      }
      logerrprintb();
    }
    const uint32_t var_wts = (pca_flags / kfPcaVarWts) & 1;
    const uint32_t chr_col = pca_flags & kfPcaVcolChrom;
    const uint32_t ref_col = pca_flags & kfPcaVcolRef;
    const uint32_t alt1_col = pca_flags & kfPcaVcolAlt1;
    const uint32_t alt_col = pca_flags & kfPcaVcolAlt;
    const uint32_t maj_col = pca_flags & kfPcaVcolMaj;
    const uint32_t nonmaj_col = pca_flags & kfPcaVcolNonmaj;
    double* cur_var_wts = nullptr;
    double* eigval_inv_sqrts = nullptr;
    char* chr_buf = nullptr;
    uintptr_t overflow_buf_size = 3 * kMaxMediumLine;
    if (var_wts) {
      if (bigstack_alloc_d(pc_ct, &cur_var_wts) ||
          bigstack_alloc_d(pc_ct, &eigval_inv_sqrts)) {
        goto calc_pca_ret_NOMEM;
      }
      uint32_t max_chr_blen = 0;
      if (chr_col) {
        max_chr_blen = get_max_chr_slen(cip) + 1;
        if (bigstack_alloc_c(max_chr_blen, &chr_buf)) {
          goto calc_pca_ret_NOMEM;
        }
      }
      const uintptr_t overflow_buf_size2 = round_up_pow2(kCompressStreamBlock + max_chr_blen + kMaxIdSlen + 2 * max_allele_slen + 32 + 16 * pc_ct, kCacheline);
      if (overflow_buf_size2 > overflow_buf_size) {
        overflow_buf_size = overflow_buf_size2;
      }
    }
    uintptr_t writebuf_alloc = overflow_buf_size;
    if (pca_flags & kfPcaVarZs) {
      writebuf_alloc += css_wkspace_req(overflow_buf_size);
    }
    // temporary
    // todo: additional --pca-clusters allocations
    const uintptr_t* pca_sample_include = sample_include;
    const uint32_t raw_sample_ctl = BITCT_TO_WORDCT(raw_sample_ct);
    const uint32_t pca_sample_ctaw2 = QUATERCT_TO_ALIGNED_WORDCT(pca_sample_ct);
    const uint32_t pca_sample_ctaw = BITCT_TO_ALIGNED_WORDCT(pca_sample_ct);
    uint32_t* pca_sample_include_cumulative_popcounts;
    double* eigvals;
    if (bigstack_alloc_ui(raw_sample_ctl, &pca_sample_include_cumulative_popcounts) ||
        bigstack_alloc_d(pc_ct, &eigvals) ||
        bigstack_alloc_thread(calc_thread_ct, &ts.threads) ||
        bigstack_alloc_dp(calc_thread_ct, &g_yy_bufs)) {
      goto calc_pca_ret_NOMEM;
    }
    fill_cumulative_popcounts(pca_sample_include, raw_sample_ctl, pca_sample_include_cumulative_popcounts);
    g_pca_sample_ct = pca_sample_ct;
    g_pc_ct = pc_ct;
    g_error_ret = kPglRetSuccess;
    uint32_t cur_allele_ct = 2;
    double* qq = nullptr;
    double* eigvecs_smaj;
    char* writebuf;
    if (is_approx) {
      if (pca_sample_ct <= 5000) {
        logerrprint("Warning: --pca approx is only recommended for analysis of >5000 samples.\n");
      }
      if (variant_ct > 5000000) {
        logerrprint("Warning: Use of --pca approx on >5m variants is not advisable.  Apply a MAF\nfilter if you haven't done so yet, and consider LD-pruning your variant set as\nwell.\n");
      }
      // This is ported from EIGENSOFT 6 src/ksrc/kjg_fpca.c , which is in turn
      // primarily based on Halko N, Martinsson P, Shkolnisky Y, Tygert M
      // (2011) An Algorithm for the Principal Component Analysis of Large Data
      // Sets.
      const uintptr_t pc_ct_x2 = pc_ct * 2;
      const uintptr_t qq_col_ct = (pc_ct + 1) * pc_ct_x2;
#ifndef LAPACK_ILP64
      if ((variant_ct * S_CAST(uint64_t, qq_col_ct)) > 0x7effffff) {
        logerrprint("Error: --pca approx problem instance too large for this " PROG_NAME_STR " build.  If this\nis really the computation you want, use a " PROG_NAME_STR " build with large-matrix\nsupport.\n");
        goto calc_pca_ret_INCONSISTENT_INPUT;
      }
#endif
      const double variant_ct_recip = 1.0 / u31tod(variant_ct);

      const uintptr_t g_size = pca_sample_ct * pc_ct_x2;
      __CLPK_integer svd_rect_lwork;
#ifdef LAPACK_ILP64
      get_svd_rect_lwork(MAXV(pca_sample_ct, variant_ct), qq_col_ct, &svd_rect_lwork);
#else
      if (get_svd_rect_lwork(MAXV(pca_sample_ct, variant_ct), qq_col_ct, &svd_rect_lwork)) {
        logerrprint("Error: --pca approx problem instance too large for this " PROG_NAME_STR " build.  If this\nis really the computation you want, use a " PROG_NAME_STR " build with large-matrix\nsupport.\n");
        goto calc_pca_ret_INCONSISTENT_INPUT;
      }
#endif
      uintptr_t svd_rect_wkspace_size = (svd_rect_lwork + qq_col_ct * qq_col_ct) * sizeof(double);
      if (svd_rect_wkspace_size < writebuf_alloc) {
        // used as writebuf later
        svd_rect_wkspace_size = writebuf_alloc;
      }

      unsigned char* svd_rect_wkspace;
      double* ss;
      double* g1;
      if (bigstack_alloc_d(qq_col_ct, &ss) ||
          bigstack_alloc_d(variant_ct * qq_col_ct, &qq) ||
          bigstack_alloc_dp(calc_thread_ct, &g_y_transpose_bufs) ||
          bigstack_alloc_dp(calc_thread_ct, &g_g2_bb_part_bufs) ||
          bigstack_alloc_uc(svd_rect_wkspace_size, &svd_rect_wkspace) ||
          bigstack_alloc_d(g_size, &g1)) {
        goto calc_pca_ret_NOMEM;
      }
      const uintptr_t genovecs_alloc = round_up_pow2(pca_sample_ctaw2 * kPcaVariantBlockSize * sizeof(intptr_t), kCacheline);
      const uintptr_t dosage_cts_alloc = round_up_pow2(kPcaVariantBlockSize * sizeof(int32_t), kCacheline);
      const uintptr_t dosage_presents_alloc = round_up_pow2(pca_sample_ctaw * kPcaVariantBlockSize * sizeof(intptr_t), kCacheline);
      const uintptr_t dosage_vals_alloc = round_up_pow2(pca_sample_ct * kPcaVariantBlockSize * sizeof(dosage_t), kCacheline);
      const uintptr_t cur_maj_freqs_alloc = round_up_pow2(kPcaVariantBlockSize * sizeof(double), kCacheline);
      const uintptr_t yy_alloc = round_up_pow2(kPcaVariantBlockSize * pca_sample_ct * sizeof(double), kCacheline);
      const uintptr_t b_size = pca_sample_ct * qq_col_ct;
      const uintptr_t g2_bb_part_alloc = round_up_pow2(b_size * sizeof(double), kCacheline);
      const uintptr_t per_thread_alloc = 2 * (genovecs_alloc + dosage_cts_alloc + dosage_presents_alloc + dosage_vals_alloc + cur_maj_freqs_alloc + yy_alloc) + g2_bb_part_alloc;

      const uintptr_t bigstack_avail = bigstack_left();
      if (per_thread_alloc * calc_thread_ct > bigstack_avail) {
        if (bigstack_avail < per_thread_alloc) {
          goto calc_pca_ret_NOMEM;
        }
        calc_thread_ct = bigstack_avail / per_thread_alloc;
      }
      for (uint32_t parity = 0; parity < 2; ++parity) {
        g_genovecs[parity] = S_CAST(uintptr_t*, bigstack_alloc_raw(genovecs_alloc * calc_thread_ct));
        g_dosage_cts[parity] = S_CAST(uint32_t*, bigstack_alloc_raw(dosage_cts_alloc * calc_thread_ct));
        g_dosage_presents[parity] = S_CAST(uintptr_t*, bigstack_alloc_raw(dosage_presents_alloc * calc_thread_ct));
        g_dosage_val_bufs[parity] = S_CAST(dosage_t*, bigstack_alloc_raw(dosage_vals_alloc * calc_thread_ct));
        g_cur_maj_freqs[parity] = S_CAST(double*, bigstack_alloc_raw(cur_maj_freqs_alloc * calc_thread_ct));
      }
      for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
        g_yy_bufs[tidx] = S_CAST(double*, bigstack_alloc_raw(yy_alloc));
        g_y_transpose_bufs[tidx] = S_CAST(double*, bigstack_alloc_raw(yy_alloc));
        g_g2_bb_part_bufs[tidx] = S_CAST(double*, bigstack_alloc_raw(g2_bb_part_alloc));
      }
      fill_gaussian_darray(g_size / 2, max_thread_ct, g1);
      g_g1 = g1;
#ifdef __APPLE__
      fputs("Projecting random vectors... ", stdout);
#else
      printf("Projecting random vectors (%u compute thread%s)... ", calc_thread_ct, (calc_thread_ct == 1)? "" : "s");
#endif
      fflush(stdout);
      pgr_clear_ld_cache(simple_pgrp);
      for (uint32_t iter_idx = 0; iter_idx <= pc_ct; ++iter_idx) {
        // kjg_fpca_XTXA(), kjg_fpca_XA()
        for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
          fill_double_zero(g_size, g_g2_bb_part_bufs[tidx]);
        }
        double* qq_iter = &(qq[iter_idx * pc_ct_x2]); // offset on first row
        g_qq = qq_iter;

        // Main workflow:
        // 1. Set n=0, load batch 0
        //
        // 2. Spawn threads processing batch n
        // 3. Increment n by 1
        // 4. Load batch n unless eof
        // 5. Join threads
        // 6. Goto step 2 unless eof
        //
        // 7. Assemble next g1 by summing g2_parts
        uint32_t parity = 0;
        uint32_t cur_variant_idx_start = 0;
        uint32_t variant_uidx = 0;
        while (1) {
          uint32_t cur_batch_size = 0;
          if (!ts.is_last_block) {
            cur_batch_size = calc_thread_ct * kPcaVariantBlockSize;
            uint32_t cur_variant_idx_end = cur_variant_idx_start + cur_batch_size;
            if (cur_variant_idx_end > variant_ct) {
              cur_batch_size = variant_ct - cur_variant_idx_start;
              cur_variant_idx_end = variant_ct;
            }
            uintptr_t* genovec_iter = g_genovecs[parity];
            uint32_t* dosage_ct_iter = g_dosage_cts[parity];
            uintptr_t* dosage_present_iter = g_dosage_presents[parity];
            dosage_t* dosage_vals_iter = g_dosage_val_bufs[parity];
            double* maj_freqs_write_iter = g_cur_maj_freqs[parity];
            for (uint32_t variant_idx = cur_variant_idx_start; variant_idx < cur_variant_idx_end; ++variant_uidx, ++variant_idx) {
              next_set_unsafe_ck(variant_include, &variant_uidx);
              uint32_t dosage_ct;
              uint32_t is_explicit_alt1;
              reterr = pgr_read_refalt1_genovec_dosage16_subset_unsafe(pca_sample_include, pca_sample_include_cumulative_popcounts, pca_sample_ct, variant_uidx, simple_pgrp, genovec_iter, dosage_present_iter, dosage_vals_iter, &dosage_ct, &is_explicit_alt1);
              if (reterr) {
                if (reterr == kPglRetMalformedInput) {
                  logprint("\n");
                  logerrprint("Error: Malformed .pgen file.\n");
                }
                goto calc_pca_ret_1;
              }
              const uint32_t maj_allele_idx = maj_alleles[variant_uidx];
              if (maj_allele_idx) {
                genovec_invert_unsafe(pca_sample_ct, genovec_iter);
                if (dosage_ct) {
                  biallelic_dosage16_invert(dosage_ct, dosage_vals_iter);
                }
              }
              zero_trailing_quaters(pca_sample_ct, genovec_iter);
              genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
              *dosage_ct_iter++ = dosage_ct;
              dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
              dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
              uintptr_t allele_idx_base;
              if (!variant_allele_idxs) {
                allele_idx_base = variant_uidx;
              } else {
                allele_idx_base = variant_allele_idxs[variant_uidx];
                cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - allele_idx_base;
                allele_idx_base -= variant_uidx;
              }
              // bugfix (23 Jul 2017): we already subtracted variant_uidx
              *maj_freqs_write_iter++ = get_allele_freq(&(allele_freqs[allele_idx_base]), maj_allele_idx, cur_allele_ct);
            }
          }
          if (cur_variant_idx_start) {
            join_threads3z(&ts);
            reterr = g_error_ret;
            if (reterr) {
              logprint("\n");
              logerrprint("Error: Zero-MAF variant is not actually monomorphic.  (This is possible when\ne.g. MAF is estimated from founders, but the minor allele was only observed in\nnonfounders.  In any case, you should be using e.g. --maf to filter out all\nvery-low-MAF variants, since the relationship matrix distance formula does not\nhandle them well.)\n");
              goto calc_pca_ret_1;
            }
            if (ts.is_last_block) {
              break;
            }
          }
          if (!cur_variant_idx_start) {
            if (iter_idx < pc_ct) {
              ts.thread_func_ptr = calc_pca_xtxa_thread;
            } else {
              ts.thread_func_ptr = calc_pca_xa_thread;
            }
          }
          ts.is_last_block = (cur_variant_idx_start + cur_batch_size == variant_ct);
          g_cur_batch_size = cur_batch_size;
          if (spawn_threads3z(cur_variant_idx_start, &ts)) {
            goto calc_pca_ret_THREAD_CREATE_FAIL;
          }
          cur_variant_idx_start += cur_batch_size;
          parity = 1 - parity;
        }
        if (iter_idx < pc_ct) {
          memcpy(g1, g_g2_bb_part_bufs[0], g_size * sizeof(double));
          for (uint32_t tidx = 1; tidx < calc_thread_ct; ++tidx) {
            const double* cur_g2_part = g_g2_bb_part_bufs[tidx];
            for (uintptr_t ulii = 0; ulii < g_size; ++ulii) {
              g1[ulii] += cur_g2_part[ulii];
            }
          }
          for (uintptr_t ulii = 0; ulii < g_size; ++ulii) {
            g1[ulii] *= variant_ct_recip;
          }
        }
#ifdef __APPLE__
        printf("\rProjecting random vectors... %u/%u", iter_idx + 1, pc_ct + 1);
#else
        printf("\rProjecting random vectors (%u compute thread%s)... %u/%u", calc_thread_ct, (calc_thread_ct == 1)? "" : "s", iter_idx + 1, pc_ct + 1);
#endif
        fflush(stdout);
      }
      fputs(".\n", stdout);
      logprint("Computing SVD of Krylov matrix... ");
      fflush(stdout);
      BLAS_SET_NUM_THREADS(max_thread_ct);
      interr_t svd_rect_err = svd_rect(variant_ct, qq_col_ct, svd_rect_lwork, qq, ss, svd_rect_wkspace);
      if (svd_rect_err) {
        logprint("\n");
        snprintf(g_logbuf, kLogbufSize, "Error: Failed to compute SVD of Krylov matrix (DGESVD info=%d).\n", S_CAST(int32_t, svd_rect_err));
        goto calc_pca_ret_INCONSISTENT_INPUT_2;
      }
      BLAS_SET_NUM_THREADS(1);
      logprint("done.\nRecovering top PCs from range approximation... ");
      fflush(stdout);

      // kjg_fpca_XTB()
      for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
        fill_double_zero(b_size, g_g2_bb_part_bufs[tidx]);
      }
      uint32_t parity = 0;
      uint32_t cur_variant_idx_start = 0;
      uint32_t variant_uidx = 0;
      reinit_threads3z(&ts);
      g_qq = qq;
      while (1) {
        uint32_t cur_batch_size = 0;
        if (!ts.is_last_block) {
          // probable todo: move this boilerplate in its own function
          cur_batch_size = calc_thread_ct * kPcaVariantBlockSize;
          uint32_t cur_variant_idx_end = cur_variant_idx_start + cur_batch_size;
          if (cur_variant_idx_end > variant_ct) {
            cur_batch_size = variant_ct - cur_variant_idx_start;
            cur_variant_idx_end = variant_ct;
          }
          uintptr_t* genovec_iter = g_genovecs[parity];
          uint32_t* dosage_ct_iter = g_dosage_cts[parity];
          uintptr_t* dosage_present_iter = g_dosage_presents[parity];
          dosage_t* dosage_vals_iter = g_dosage_val_bufs[parity];
          double* maj_freqs_write_iter = g_cur_maj_freqs[parity];
          for (uint32_t variant_idx = cur_variant_idx_start; variant_idx < cur_variant_idx_end; ++variant_uidx, ++variant_idx) {
            next_set_unsafe_ck(variant_include, &variant_uidx);
            uint32_t dosage_ct;
            uint32_t is_explicit_alt1;
            reterr = pgr_read_refalt1_genovec_dosage16_subset_unsafe(pca_sample_include, pca_sample_include_cumulative_popcounts, pca_sample_ct, variant_uidx, simple_pgrp, genovec_iter, dosage_present_iter, dosage_vals_iter, &dosage_ct, &is_explicit_alt1);
            if (reterr) {
              goto calc_pca_ret_READ_FAIL;
            }
            const uint32_t maj_allele_idx = maj_alleles[variant_uidx];
            if (maj_allele_idx) {
              genovec_invert_unsafe(pca_sample_ct, genovec_iter);
              if (dosage_ct) {
                biallelic_dosage16_invert(dosage_ct, dosage_vals_iter);
              }
            }
            zero_trailing_quaters(pca_sample_ct, genovec_iter);
            genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
            *dosage_ct_iter++ = dosage_ct;
            dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
            dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
            uintptr_t allele_idx_base;
            if (!variant_allele_idxs) {
              allele_idx_base = variant_uidx;
            } else {
              allele_idx_base = variant_allele_idxs[variant_uidx];
              cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - allele_idx_base;
              allele_idx_base -= variant_uidx;
            }
            *maj_freqs_write_iter++ = get_allele_freq(&(allele_freqs[allele_idx_base]), maj_allele_idx, cur_allele_ct);
          }
        }
        if (cur_variant_idx_start) {
          join_threads3z(&ts);
          if (g_error_ret) {
            // this error *didn't* happen on an earlier pass, so assign blame
            // to I/O instead
            goto calc_pca_ret_READ_FAIL;
          }
          if (ts.is_last_block) {
            break;
          }
        }
        ts.is_last_block = (cur_variant_idx_start + cur_batch_size == variant_ct);
        g_cur_batch_size = cur_batch_size;
        ts.thread_func_ptr = calc_pca_xtb_thread;
        if (spawn_threads3z(cur_variant_idx_start, &ts)) {
          goto calc_pca_ret_THREAD_CREATE_FAIL;
        }
        cur_variant_idx_start += cur_batch_size;
        parity = 1 - parity;
      }
      double* bb = g_g2_bb_part_bufs[0];
      for (uint32_t tidx = 1; tidx < calc_thread_ct; ++tidx) {
        const double* cur_bb_part = g_g2_bb_part_bufs[tidx];
        for (uintptr_t ulii = 0; ulii < b_size; ++ulii) {
          bb[ulii] += cur_bb_part[ulii];
        }
      }
      BLAS_SET_NUM_THREADS(max_thread_ct);
      svd_rect_err = svd_rect(pca_sample_ct, qq_col_ct, svd_rect_lwork, bb, ss, svd_rect_wkspace);
      if (svd_rect_err) {
        logprint("\n");
        snprintf(g_logbuf, kLogbufSize, "Error: Failed to compute SVD of final matrix (DGESVD info=%d).\n", S_CAST(int32_t, svd_rect_err));
        goto calc_pca_ret_INCONSISTENT_INPUT_2;
      }
      BLAS_SET_NUM_THREADS(1);
      logprint("done.\n");
      eigvecs_smaj = g1;
      for (uint32_t sample_idx = 0; sample_idx < pca_sample_ct; ++sample_idx) {
        memcpy(&(eigvecs_smaj[sample_idx * S_CAST(uintptr_t, pc_ct)]), &(bb[sample_idx * qq_col_ct]), pc_ct * sizeof(double));
      }
      for (uint32_t pc_idx = 0; pc_idx < pc_ct; ++pc_idx) {
        eigvals[pc_idx] = ss[pc_idx] * ss[pc_idx] * variant_ct_recip;
      }
      writebuf = R_CAST(char*, svd_rect_wkspace);
    } else {
      __CLPK_integer lwork;
      __CLPK_integer liwork;
      uintptr_t wkspace_byte_ct;
      if (get_extract_eigvecs_lworks(pca_sample_ct, pc_ct, &lwork, &liwork, &wkspace_byte_ct)) {
        goto calc_pca_ret_NOMEM;
      }
      const uintptr_t eigvecs_smaj_alloc = pc_ct * pca_sample_ct * sizeof(double);
      if (wkspace_byte_ct < eigvecs_smaj_alloc) {
        wkspace_byte_ct = eigvecs_smaj_alloc;
      }
      double* reverse_eigvecs_pcmaj;
      unsigned char* extract_eigvecs_wkspace;
      if (bigstack_alloc_d(pc_ct * pca_sample_ct, &reverse_eigvecs_pcmaj) ||
          bigstack_alloc_uc(wkspace_byte_ct, &extract_eigvecs_wkspace)) {
        goto calc_pca_ret_NOMEM;
      }
      LOGPRINTF("Extracting eigenvalue%s and eigenvector%s... ", (pc_ct == 1)? "" : "s", (pc_ct == 1)? "" : "s");
      fflush(stdout);
      BLAS_SET_NUM_THREADS(max_thread_ct);
      if (extract_eigvecs(pca_sample_ct, pc_ct, lwork, liwork, grm, eigvals, reverse_eigvecs_pcmaj, extract_eigvecs_wkspace)) {
        logerrprint("Error: Failed to extract eigenvector(s) from GRM.\n");
        goto calc_pca_ret_INCONSISTENT_INPUT;
      }
      BLAS_SET_NUM_THREADS(1);
      logprint("done.\n");
      eigvecs_smaj = R_CAST(double*, extract_eigvecs_wkspace);
      bigstack_shrink_top(eigvecs_smaj, eigvecs_smaj_alloc);
      if (bigstack_alloc_c(writebuf_alloc, &writebuf)) {
        goto calc_pca_ret_NOMEM;
      }

      // extract_eigvecs() results are in reverse order, and we also need to
      // transpose eigenvectors to sample-major
      const uint32_t pc_ct_m1 = pc_ct - 1;
      const uint32_t pc_ct_div2 = pc_ct / 2;
      for (uint32_t pc_idx = 0; pc_idx < pc_ct_div2; ++pc_idx) {
        double tmp_eigval = eigvals[pc_idx];
        eigvals[pc_idx] = eigvals[pc_ct_m1 - pc_idx];
        eigvals[pc_ct_m1 - pc_idx] = tmp_eigval;
      }
      double* eigvecs_smaj_iter = eigvecs_smaj;
      for (uint32_t sample_idx = 0; sample_idx < pca_sample_ct; ++sample_idx) {
        uintptr_t pc_inv_idx = pc_ct;
        const double* reverse_eigvecs_col = &(reverse_eigvecs_pcmaj[sample_idx]);
        do {
          --pc_inv_idx;
          *eigvecs_smaj_iter++ = reverse_eigvecs_col[pc_inv_idx * pca_sample_ct];
        } while (pc_inv_idx);
      }
    }
    // (later: --pca-cluster-names, --pca-clusters)
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);

    if (var_wts) {
      g_g1 = eigvecs_smaj;
      for (uint32_t pc_idx = 0; pc_idx < pc_ct; ++pc_idx) {
        eigval_inv_sqrts[pc_idx] = 1.0 / sqrt(eigvals[pc_idx]);
      }

      const uint32_t output_zst = (pca_flags / kfPcaVarZs) & 1;
      outname_zst_set(".eigenvec.var", output_zst, outname_end);
      reterr = cswrite_init(outname, 0, output_zst, max_thread_ct, overflow_buf_size, writebuf, R_CAST(unsigned char*, &(writebuf[overflow_buf_size])), &css);
      if (reterr) {
        goto calc_pca_ret_1;
      }
      cswritep = writebuf;
      *cswritep++ = '#';
      if (chr_col) {
        cswritep = strcpya(cswritep, "CHROM\t");
      }
      if (pca_flags & kfPcaVcolPos) {
        cswritep = strcpya(cswritep, "POS\t");
      } else {
        variant_bps = nullptr;
      }
      cswritep = strcpya(cswritep, "ID");
      if (ref_col) {
        cswritep = strcpya(cswritep, "\tREF");
      }
      if (alt1_col) {
        cswritep = strcpya(cswritep, "\tALT1");
      }
      if (alt_col) {
        cswritep = strcpya(cswritep, "\tALT");
      }
      if (maj_col) {
        cswritep = strcpya(cswritep, "\tMAJ");
      }
      if (nonmaj_col) {
        cswritep = strcpya(cswritep, "\tNONMAJ");
      }
      for (uint32_t pc_idx = 0; pc_idx < pc_ct;) {
        ++pc_idx;
        cswritep = memcpyl3a(cswritep, "\tPC");
        cswritep = uint32toa(pc_idx, cswritep);
      }
      append_binary_eoln(&cswritep);

      // Main workflow:
      // 1. Set n=0, load batch 0
      //
      // 2. Spawn threads processing batch n
      // 3. If n>0, write results and update projection for block (n-1)
      // 4. Increment n by 1
      // 5. Load batch n unless eof
      // 6. Join threads
      // 7. Goto step 2 unless eof
      //
      // 8. Write results and update projection for last block
      uint32_t cur_variant_idx_start = 0;
#ifndef __APPLE__
      if (output_zst) {
        // compression is relatively expensive?
        calc_thread_ct = 1;
        ts.calc_thread_ct = 1;
      }
#endif
      uintptr_t var_wts_part_size;
      if (qq) {
        var_wts_part_size = (MINV(variant_ct, calc_thread_ct * kPcaVariantBlockSize)) * S_CAST(uintptr_t, pc_ct);
      } else {
        // non-approximate PCA, bunch of buffers have not been allocated yet

        // if grm[] (which we no longer need) has at least as much remaining
        // space as bigstack, allocate from grm
        unsigned char* arena_bottom = R_CAST(unsigned char*, grm);
        unsigned char* arena_top = bigstack_mark;
        uintptr_t arena_avail = arena_top - arena_bottom;
        if (arena_avail < bigstack_left()) {
          arena_bottom = g_bigstack_base;
          arena_top = g_bigstack_end;
          arena_avail = bigstack_left();
        }
        const uintptr_t var_wts_part_alloc = round_up_pow2(2 * kPcaVariantBlockSize * sizeof(double) * pc_ct, kCacheline);
        const uintptr_t genovecs_alloc = round_up_pow2(pca_sample_ctaw2 * kPcaVariantBlockSize * sizeof(intptr_t), kCacheline);
        const uintptr_t dosage_cts_alloc = round_up_pow2(kPcaVariantBlockSize * sizeof(int32_t), kCacheline);
        const uintptr_t dosage_presents_alloc = round_up_pow2(pca_sample_ctaw * kPcaVariantBlockSize * sizeof(intptr_t), kCacheline);
        const uintptr_t dosage_vals_alloc = round_up_pow2(pca_sample_ct * kPcaVariantBlockSize * sizeof(dosage_t), kCacheline);
        const uintptr_t cur_maj_freqs_alloc = round_up_pow2(kPcaVariantBlockSize * sizeof(double), kCacheline);
        const uintptr_t yy_alloc = round_up_pow2(kPcaVariantBlockSize * pca_sample_ct * sizeof(double), kCacheline);
        const uintptr_t per_thread_alloc = 2 * (genovecs_alloc + dosage_cts_alloc + dosage_presents_alloc + dosage_vals_alloc + cur_maj_freqs_alloc) + yy_alloc + var_wts_part_alloc;
        if (per_thread_alloc * calc_thread_ct > arena_avail) {
          if (arena_avail < per_thread_alloc) {
            goto calc_pca_ret_NOMEM;
          }
          calc_thread_ct = arena_avail / per_thread_alloc;
        }
        ts.calc_thread_ct = calc_thread_ct;
        for (uint32_t parity = 0; parity < 2; ++parity) {
          g_genovecs[parity] = S_CAST(uintptr_t*, arena_alloc_raw(genovecs_alloc * calc_thread_ct, &arena_bottom));
          g_dosage_cts[parity] = S_CAST(uint32_t*, arena_alloc_raw(dosage_cts_alloc * calc_thread_ct, &arena_bottom));
          g_dosage_presents[parity] = S_CAST(uintptr_t*, arena_alloc_raw(dosage_presents_alloc * calc_thread_ct, &arena_bottom));
          g_dosage_val_bufs[parity] = S_CAST(dosage_t*, arena_alloc_raw(dosage_vals_alloc * calc_thread_ct, &arena_bottom));
          g_cur_maj_freqs[parity] = S_CAST(double*, arena_alloc_raw(cur_maj_freqs_alloc * calc_thread_ct, &arena_bottom));
        }
        for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
          g_yy_bufs[tidx] = S_CAST(double*, arena_alloc_raw(yy_alloc, &arena_bottom));
        }
        var_wts_part_size = (MINV(variant_ct, calc_thread_ct * kPcaVariantBlockSize)) * S_CAST(uintptr_t, pc_ct);
        qq = S_CAST(double*, arena_alloc_raw_rd(2 * var_wts_part_size * sizeof(double), &arena_bottom));
        g_qq = qq;
#ifndef NDEBUG
        if (arena_top == g_bigstack_end) {
          // we shouldn't make any more allocations, but just in case...
          g_bigstack_base = arena_bottom;
        }
#endif
      }
      uint32_t prev_batch_size = 0;
      uint32_t variant_uidx = next_set_unsafe(variant_include, 0);
      uint32_t variant_uidx_load = variant_uidx;
      uint32_t parity = 0;
      reinit_threads3z(&ts);
      uint32_t chr_fo_idx = UINT32_MAX;
      uint32_t chr_end = 0;
      uint32_t chr_buf_blen = 0;
      while (1) {
        uint32_t cur_batch_size = 0;
        if (!ts.is_last_block) {
          cur_batch_size = calc_thread_ct * kPcaVariantBlockSize;
          uint32_t cur_variant_idx_end = cur_variant_idx_start + cur_batch_size;
          if (cur_variant_idx_end > variant_ct) {
            cur_batch_size = variant_ct - cur_variant_idx_start;
            cur_variant_idx_end = variant_ct;
          }
          uintptr_t* genovec_iter = g_genovecs[parity];
          uint32_t* dosage_ct_iter = g_dosage_cts[parity];
          uintptr_t* dosage_present_iter = g_dosage_presents[parity];
          dosage_t* dosage_vals_iter = g_dosage_val_bufs[parity];
          double* maj_freqs_write_iter = g_cur_maj_freqs[parity];
          for (uint32_t variant_idx = cur_variant_idx_start; variant_idx < cur_variant_idx_end; ++variant_uidx_load, ++variant_idx) {
            next_set_unsafe_ck(variant_include, &variant_uidx_load);
            uint32_t dosage_ct;
            uint32_t is_explicit_alt1;
            reterr = pgr_read_refalt1_genovec_dosage16_subset_unsafe(pca_sample_include, pca_sample_include_cumulative_popcounts, pca_sample_ct, variant_uidx_load, simple_pgrp, genovec_iter, dosage_present_iter, dosage_vals_iter, &dosage_ct, &is_explicit_alt1);
            if (reterr) {
              goto calc_pca_ret_READ_FAIL;
            }
            const uint32_t maj_allele_idx = maj_alleles[variant_uidx_load];
            if (maj_allele_idx) {
              genovec_invert_unsafe(pca_sample_ct, genovec_iter);
              if (dosage_ct) {
                biallelic_dosage16_invert(dosage_ct, dosage_vals_iter);
              }
            }
            zero_trailing_quaters(pca_sample_ct, genovec_iter);
            genovec_iter = &(genovec_iter[pca_sample_ctaw2]);
            *dosage_ct_iter++ = dosage_ct;
            dosage_present_iter = &(dosage_present_iter[pca_sample_ctaw]);
            dosage_vals_iter = &(dosage_vals_iter[pca_sample_ct]);
            uintptr_t allele_idx_base;
            if (!variant_allele_idxs) {
              allele_idx_base = variant_uidx_load;
            } else {
              allele_idx_base = variant_allele_idxs[variant_uidx_load];
              cur_allele_ct = variant_allele_idxs[variant_uidx_load + 1] - allele_idx_base;
              allele_idx_base -= variant_uidx_load;
            }
            *maj_freqs_write_iter++ = get_allele_freq(&(allele_freqs[allele_idx_base]), maj_allele_idx, cur_allele_ct);
          }
        }
        if (cur_variant_idx_start) {
          join_threads3z(&ts);
          if (g_error_ret) {
            goto calc_pca_ret_READ_FAIL;
          }
        }
        if (!ts.is_last_block) {
          g_cur_batch_size = cur_batch_size;
          ts.is_last_block = (cur_variant_idx_start + cur_batch_size == variant_ct);
          ts.thread_func_ptr = calc_pca_var_wts_thread;
          if (spawn_threads3z(cur_variant_idx_start, &ts)) {
            goto calc_pca_ret_THREAD_CREATE_FAIL;
          }
        }
        parity = 1 - parity;
        if (cur_variant_idx_start) {
          // write *previous* block results
          const double* var_wts_iter = &(qq[parity * var_wts_part_size]);
          // (todo: update projection here)
          for (uint32_t vidx = cur_variant_idx_start - prev_batch_size; vidx < cur_variant_idx_start; ++vidx, ++variant_uidx) {
            next_set_unsafe_ck(variant_include, &variant_uidx);
            if (chr_col) {
              if (variant_uidx >= chr_end) {
                int32_t chr_idx;
                do {
                  ++chr_fo_idx;
                  chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
                } while (variant_uidx >= chr_end);
                chr_idx = cip->chr_file_order[chr_fo_idx];
                char* chr_name_end = chr_name_write(cip, chr_idx, chr_buf);
                *chr_name_end = '\t';
                chr_buf_blen = 1 + S_CAST(uintptr_t, chr_name_end - chr_buf);
              }
              cswritep = memcpya(cswritep, chr_buf, chr_buf_blen);
            }
            if (variant_bps) {
              cswritep = uint32toa_x(variant_bps[variant_uidx], '\t', cswritep);
            }
            cswritep = strcpya(cswritep, variant_ids[variant_uidx]);
            uintptr_t variant_allele_idx_base = variant_uidx * 2;
            if (variant_allele_idxs) {
              variant_allele_idx_base = variant_allele_idxs[variant_uidx];
              cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - variant_allele_idx_base;
            }
            const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
            if (ref_col) {
              *cswritep++ = '\t';
              cswritep = strcpya(cswritep, cur_alleles[0]);
            }
            if (alt1_col) {
              *cswritep++ = '\t';
              cswritep = strcpya(cswritep, cur_alleles[1]);
            }
            if (alt_col) {
              *cswritep++ = '\t';
              for (uint32_t allele_idx = 1; allele_idx < cur_allele_ct; ++allele_idx) {
                if (cswrite(&css, &cswritep)) {
                  goto calc_pca_ret_WRITE_FAIL;
                }
                cswritep = strcpyax(cswritep, cur_alleles[allele_idx], ',');
              }
              --cswritep;
            }
            const uint32_t maj_allele_idx = maj_alleles[variant_uidx];
            if (maj_col) {
              if (cswrite(&css, &cswritep)) {
                goto calc_pca_ret_WRITE_FAIL;
              }
              *cswritep++ = '\t';
              cswritep = strcpya(cswritep, cur_alleles[maj_allele_idx]);
            }
            if (nonmaj_col) {
              *cswritep++ = '\t';
              for (uint32_t allele_idx = 0; allele_idx < cur_allele_ct; ++allele_idx) {
                if (allele_idx == maj_allele_idx) {
                  continue;
                }
                if (cswrite(&css, &cswritep)) {
                  goto calc_pca_ret_WRITE_FAIL;
                }
                cswritep = strcpyax(cswritep, cur_alleles[allele_idx], ',');
              }
              --cswritep;
            }
            for (uint32_t pc_idx = 0; pc_idx < pc_ct; ++pc_idx) {
              *cswritep++ = '\t';
              // could avoid these multiplications by premultiplying the
              // sample weight matrix
              cswritep = dtoa_g((*var_wts_iter++) * eigval_inv_sqrts[pc_idx], cswritep);
            }
            append_binary_eoln(&cswritep);
            if (cswrite(&css, &cswritep)) {
              // bugfix (15 Dec 2017): prevent buffer overflow when ALT, MAJ,
              // and NONMAJ columns all missing.
              goto calc_pca_ret_WRITE_FAIL;
            }
          }
        }
        if (cur_variant_idx_start == variant_ct) {
          break;
        }
        cur_variant_idx_start += cur_batch_size;
        prev_batch_size = cur_batch_size;
      }
      if (cswrite_close_null(&css, cswritep)) {
        goto calc_pca_ret_WRITE_FAIL;
      }
      LOGPRINTFWW("--pca%s: Variant weights written to %s .\n", is_approx? " approx" : "", outname);
    }

    snprintf(outname_end, kMaxOutfnameExtBlen, ".eigenvec");
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto calc_pca_ret_OPEN_FAIL;
    }
    char* write_iter = strcpya(writebuf, "#FID\tIID");
    if (sids) {
      write_iter = strcpya(write_iter, "\tSID");
    }
    for (uint32_t pc_idx = 0; pc_idx < pc_ct;) {
      ++pc_idx;
      write_iter = memcpyl3a(write_iter, "\tPC");
      write_iter = uint32toa(pc_idx, write_iter);
    }
    append_binary_eoln(&write_iter);
    const uint32_t sample_ct = pca_sample_ct;
    uint32_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
      next_set_unsafe_ck(sample_include, &sample_uidx);
      write_iter = strcpya(write_iter, &(sample_ids[sample_uidx * max_sample_id_blen]));
      if (sids) {
        *write_iter++ = '\t';
        write_iter = strcpya(write_iter, &(sids[sample_uidx * max_sid_blen]));
      }
      double* sample_wts_iter = &(eigvecs_smaj[sample_idx * pc_ct]);
      // todo: read from proj_sample_wts instead when pca_sample_include bit
      // not set
      for (uint32_t pc_idx = 0; pc_idx < pc_ct; ++pc_idx) {
        *write_iter++ = '\t';
        write_iter = dtoa_g(*sample_wts_iter++, write_iter);
      }
      append_binary_eoln(&write_iter);
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto calc_pca_ret_WRITE_FAIL;
      }
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto calc_pca_ret_WRITE_FAIL;
    }

    snprintf(outname_end, kMaxOutfnameExtBlen, ".eigenval");
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto calc_pca_ret_OPEN_FAIL;
    }
    write_iter = writebuf;
    for (uint32_t pc_idx = 0; pc_idx < pc_ct; ++pc_idx) {
      write_iter = dtoa_g(eigvals[pc_idx], write_iter);
      append_binary_eoln(&write_iter);
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto calc_pca_ret_WRITE_FAIL;
    }
    *outname_end = '\0';
    LOGPRINTFWW("--pca%s: Eigenvector%s written to %s.eigenvec , and eigenvalue%s written to %s.eigenval .\n", is_approx? " approx" : "", (pc_ct == 1)? "" : "s", outname, (pc_ct == 1)? "" : "s", outname);
  }
  while (0) {
  calc_pca_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  calc_pca_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  calc_pca_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  calc_pca_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  calc_pca_ret_INCONSISTENT_INPUT_2:
    logerrprintb();
  calc_pca_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  calc_pca_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 calc_pca_ret_1:
  threads3z_cleanup(&ts, &g_cur_batch_size);
  BLAS_SET_NUM_THREADS(1);
  cswrite_close_cond(&css, cswritep);
  fclose_cond(outfile);
  if (grm) {
    // nothing after --pca in the plink2 order of operations uses grm[]
    bigstack_reset(grm);
  } else {
    bigstack_reset(bigstack_mark);
  }
  return reterr;
}
#endif

// to test: do we actually want cur_dosage_ints to be uint64_t* instead of
// uint32_t*?
void fill_cur_dosage_ints(const uintptr_t* genovec_buf, const uintptr_t* dosage_present, const dosage_t* dosage_vals_buf, uint32_t sample_ct, uint32_t dosage_ct, uint32_t is_diploid_p1, uint64_t* cur_dosage_ints) {
  const uint32_t sample_ctl2_m1 = (sample_ct - 1) / kBitsPerWordD2;
  uint32_t loop_len = kBitsPerWordD2;
  uint32_t widx = 0;
  uint64_t lookup_table[4];
  lookup_table[0] = 0;
  lookup_table[1] = is_diploid_p1 * kDosageMid;
  lookup_table[2] = is_diploid_p1 * kDosageMax;
  lookup_table[3] = 0;
  uint64_t* cur_dosage_ints_iter = cur_dosage_ints;
  while (1) {
    if (widx >= sample_ctl2_m1) {
      if (widx > sample_ctl2_m1) {
        break;
      }
      // bugfix (6 Oct 2017): this needs to be MOD_NZ, not 1 + MOD_NZ
      loop_len = MOD_NZ(sample_ct, kBitsPerWordD2);
    }
    uintptr_t cur_geno_word = genovec_buf[widx];
    for (uint32_t uii = 0; uii < loop_len; ++uii) {
      const uintptr_t cur_geno = cur_geno_word & 3;
      *cur_dosage_ints_iter++ = lookup_table[cur_geno];
      cur_geno_word >>= 2;
    }
    ++widx;
  }
  uint32_t sample_idx = 0;
  for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_idx) {
    next_set_unsafe_ck(dosage_present, &sample_idx);
    cur_dosage_ints[sample_idx] = dosage_vals_buf[dosage_idx] * is_diploid_p1;
  }
}

CONSTU31(kScoreVariantBlockSize, 240);
static double* g_dosages_vmaj[2] = {nullptr, nullptr};
static double* g_score_coefs_cmaj[2] = {nullptr, nullptr};
// don't bother to explicitly multithread for now
static double* g_final_scores_cmaj = nullptr;
static uint32_t g_score_col_ct = 0;
static uint32_t g_sample_ct = 0;

THREAD_FUNC_DECL calc_score_thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  assert(!tidx);
  double* final_scores_cmaj = g_final_scores_cmaj;
  const uint32_t score_col_ct = g_score_col_ct;
  const uint32_t sample_ct = g_sample_ct;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_batch = g_is_last_thread_block;
    const uint32_t cur_batch_size = g_cur_batch_size;
    if (cur_batch_size) {
      row_major_matrix_multiply_strided_incr(g_score_coefs_cmaj[parity], g_dosages_vmaj[parity], score_col_ct, kScoreVariantBlockSize, sample_ct, sample_ct, cur_batch_size, sample_ct, final_scores_cmaj);
    }
    if (is_last_batch) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

pglerr_t score_report(const uintptr_t* sample_include, const char* sample_ids, const char* sids, const uintptr_t* sex_male, const pheno_col_t* pheno_cols, const char* pheno_names, const uintptr_t* variant_include, const chr_info_t* cip, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const double* allele_freqs, const score_info_t* score_info_ptr, uint32_t raw_sample_ct, uint32_t sample_ct, uintptr_t max_sample_id_blen, uintptr_t max_sid_blen, uint32_t pheno_ct, uintptr_t max_pheno_name_blen, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_variant_id_slen, uint32_t xchr_model, uint32_t max_thread_ct, pgen_reader_t* simple_pgrp, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  unsigned char* bigstack_end_mark = g_bigstack_end;
  gzFile gz_infile = nullptr;
  uintptr_t loadbuf_size = 0;
  uintptr_t line_idx = 0;
  threads_state_t ts;
  init_threads3z(&ts);
  char* cswritep = nullptr;
  compress_stream_state_t css;
  cswrite_init_null(&css);
  pglerr_t reterr = kPglRetSuccess;
  {
    const uint32_t raw_variant_ctl = BITCT_TO_WORDCT(raw_variant_ct);
    if (!xchr_model) {
      int32_t x_code;
      if (xymt_exists(cip, kChrOffsetX, &x_code)) {
        uint32_t x_chr_fo_idx = cip->chr_idx_to_foidx[S_CAST(uint32_t, x_code)];
        uint32_t x_start = cip->chr_fo_vidx_start[x_chr_fo_idx];
        uint32_t x_end = cip->chr_fo_vidx_start[x_chr_fo_idx + 1];
        if (!are_all_bits_zero(variant_include, x_start, x_end)) {
          uintptr_t* variant_include_no_x;
          if (bigstack_alloc_ul(raw_variant_ctl, &variant_include_no_x)) {
            goto score_report_ret_NOMEM;
          }
          memcpy(variant_include_no_x, variant_include, raw_variant_ctl * sizeof(intptr_t));
          clear_bits_nz(x_start, x_end, variant_include_no_x);
          variant_include = variant_include_no_x;
        }
      }
    } else if (xchr_model == 2) {
      xchr_model = 0;
    }
    // now xchr_model is set iff it's 1

    const score_flags_t score_flags = score_info_ptr->flags;
    reterr = gzopen_read_checked(score_info_ptr->input_fname, &gz_infile);
    if (reterr) {
      goto score_report_ret_1;
    }

    loadbuf_size = bigstack_left() / 8;
    if (loadbuf_size > kMaxLongLine) {
      loadbuf_size = kMaxLongLine;
    } else {
      loadbuf_size &= ~(kCacheline - 1);
      if (loadbuf_size <= kMaxMediumLine) {
        goto score_report_ret_NOMEM;
      }
    }
    char* loadbuf = S_CAST(char*, bigstack_alloc_raw(loadbuf_size));
    loadbuf[loadbuf_size - 1] = ' ';
    char* loadbuf_first_token;
    uint32_t lines_to_skip_p1 = 1 + ((score_flags / kfScoreHeaderIgnore) & 1);
    for (uint32_t uii = 0; uii < lines_to_skip_p1; ++uii) {
      do {
        if (!gzgets(gz_infile, loadbuf, loadbuf_size)) {
          if (!gzeof(gz_infile)) {
            goto score_report_ret_READ_FAIL;
          }
          logerrprint("Error: Empty --score file.\n");
          goto score_report_ret_MALFORMED_INPUT;
        }
        ++line_idx;
        if (!loadbuf[loadbuf_size - 1]) {
          goto score_report_ret_LONG_LINE;
        }
        loadbuf_first_token = skip_initial_spaces(loadbuf);
      } while (is_eoln_kns(*loadbuf_first_token));
    }
    uint32_t last_col_idx = count_tokens(loadbuf_first_token);
    const uint32_t varid_col_idx = score_info_ptr->varid_col_p1 - 1;
    const uint32_t allele_col_idx = score_info_ptr->allele_col_p1 - 1;
    if (MAXV(varid_col_idx, allele_col_idx) >= last_col_idx) {
      goto score_report_ret_MISSING_TOKENS;
    }
    uint32_t* score_col_idx_deltas = nullptr;
    uintptr_t score_col_ct = 1;
    if (!score_info_ptr->input_col_idx_range_list.name_ct) {
      if (allele_col_idx == last_col_idx) {
        goto score_report_ret_MISSING_TOKENS;
      }
      if (bigstack_alloc_ui(1, &score_col_idx_deltas)) {
        goto score_report_ret_NOMEM;
      }
      // catch corner case
      if (allele_col_idx + 1 == varid_col_idx) {
        logerrprint("Error: --score variant ID column index matches a coefficient column index.\n");
        goto score_report_ret_INVALID_CMDLINE;
      }
      score_col_idx_deltas[0] = allele_col_idx + 1;
    } else {
      const uint32_t last_col_idxl = BITCT_TO_WORDCT(last_col_idx);
      uintptr_t* score_col_bitarr;
      if (bigstack_end_calloc_ul(last_col_idxl, &score_col_bitarr)) {
        goto score_report_ret_NOMEM;
      }
      if (numeric_range_list_to_bitarr(&(score_info_ptr->input_col_idx_range_list), last_col_idx, 1, 0, score_col_bitarr)) {
        goto score_report_ret_MISSING_TOKENS;
      }
      if (is_set(score_col_bitarr, varid_col_idx)) {
        logerrprint("Error: --score variant ID column index matches a coefficient column index.\n");
        goto score_report_ret_INVALID_CMDLINE;
      }
      if (is_set(score_col_bitarr, allele_col_idx)) {
        logerrprint("Error: --score allele column index matches a coefficient column index.\n");
        goto score_report_ret_INVALID_CMDLINE;
      }
      score_col_ct = popcount_longs(score_col_bitarr, last_col_idxl);
      if (bigstack_alloc_ui(score_col_ct, &score_col_idx_deltas)) {
        goto score_report_ret_NOMEM;
      }
      uint32_t col_uidx = 0;
      for (uintptr_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx, ++col_uidx) {
        next_set_unsafe_ck(score_col_bitarr, &col_uidx);
        score_col_idx_deltas[score_col_idx] = col_uidx;
      }
      // now convert to deltas
      for (uintptr_t score_col_idx = score_col_ct - 1; score_col_idx; --score_col_idx) {
        score_col_idx_deltas[score_col_idx] -= score_col_idx_deltas[score_col_idx - 1];
      }
      bigstack_end_reset(bigstack_end_mark);
    }
    char** score_col_names;
    if (bigstack_alloc_cp(score_col_ct, &score_col_names)) {
      goto score_report_ret_NOMEM;
    }
    char* write_iter = R_CAST(char*, g_bigstack_base);
    // don't have to worry about overflow, since loadbuf was limited to 1/8
    // of available workspace.
    if (score_flags & kfScoreHeaderRead) {
      char* read_iter = loadbuf_first_token;
      for (uintptr_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
        read_iter = next_token_multz(read_iter, score_col_idx_deltas[score_col_idx]);
        score_col_names[score_col_idx] = write_iter;
        char* token_end = token_endnn(read_iter);
        const uint32_t slen = token_end - read_iter;
        write_iter = memcpyax(write_iter, read_iter, slen, '\0');
      }

      // don't reparse this line
      *loadbuf_first_token = '\0';
    } else {
      for (uintptr_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
        score_col_names[score_col_idx] = write_iter;
        write_iter = strcpya(write_iter, "SCORE");
        write_iter = uint32toa_x(score_col_idx + 1, '\0', write_iter);
      }
    }
    bigstack_base_set(write_iter);

    g_score_col_ct = score_col_ct;
    g_sample_ct = sample_ct;
    g_cur_batch_size = kScoreVariantBlockSize;
    ts.calc_thread_ct = 1;
    const uint32_t raw_sample_ctl = BITCT_TO_WORDCT(raw_sample_ct);
    const uint32_t sample_ctl2 = QUATERCT_TO_WORDCT(sample_ct);
    const uint32_t sample_ctl = BITCT_TO_WORDCT(sample_ct);
    const uint32_t acc1_vec_ct = BITCT_TO_VECCT(sample_ct);
    const uint32_t acc4_vec_ct = acc1_vec_ct * 4;
    const uint32_t acc8_vec_ct = acc1_vec_ct * 8;
    const uint32_t write_score_avgs = (score_flags / kfScoreColScoreAvgs) & 1;
    const uint32_t write_score_sums = (score_flags / kfScoreColScoreSums) & 1;
    const uintptr_t overflow_buf_size = round_up_pow2((score_col_ct * (write_score_avgs + write_score_sums) + pheno_ct) * 16 + 3 * kMaxIdSlen + kCompressStreamBlock + 64, kCacheline);
    uintptr_t overflow_buf_alloc = overflow_buf_size;
    if (score_flags & (kfScoreZs | kfScoreListVariantsZs)) {
      overflow_buf_alloc += css_wkspace_req(overflow_buf_size);
    }
    uint32_t* sample_include_cumulative_popcounts = nullptr;
    uintptr_t* sex_nonmale_collapsed = nullptr;
    uintptr_t* genovec_buf = nullptr;
    uintptr_t* dosage_present_buf = nullptr;
    dosage_t* dosage_vals_buf = nullptr;
    uintptr_t* missing_acc1 = nullptr;
    uintptr_t* missing_male_acc1 = nullptr;
    uint64_t* dosage_sums;
    uint64_t* dosage_incrs;
    uintptr_t* already_seen;
    char* overflow_buf = nullptr;
    if (bigstack_alloc_thread(1, &ts.threads) ||
        bigstack_alloc_d((kScoreVariantBlockSize * k1LU) * sample_ct, &(g_dosages_vmaj[0])) ||
        bigstack_alloc_d((kScoreVariantBlockSize * k1LU) * sample_ct, &(g_dosages_vmaj[1])) ||
        bigstack_alloc_d(kScoreVariantBlockSize * score_col_ct, &(g_score_coefs_cmaj[0])) ||
        bigstack_alloc_d(kScoreVariantBlockSize * score_col_ct, &(g_score_coefs_cmaj[1])) ||
        bigstack_calloc_d(score_col_ct * sample_ct, &g_final_scores_cmaj) ||
        // bugfix (4 Nov 2017): need raw_sample_ctl here, not sample_ctl
        bigstack_alloc_ui(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_ul(sample_ctl, &sex_nonmale_collapsed) ||
        bigstack_alloc_ul(sample_ctl2, &genovec_buf) ||
        bigstack_alloc_ul(sample_ctl, &dosage_present_buf) ||
        bigstack_alloc_dosage(sample_ct, &dosage_vals_buf) ||
        bigstack_alloc_ul(45 * acc1_vec_ct * kWordsPerVec, &missing_acc1) ||
        bigstack_alloc_ul(45 * acc1_vec_ct * kWordsPerVec, &missing_male_acc1) ||
        bigstack_calloc_ull(sample_ct, &dosage_sums) ||
        bigstack_calloc_ull(sample_ct, &dosage_incrs) ||
        bigstack_calloc_ul(raw_variant_ctl, &already_seen) ||
        bigstack_alloc_c(overflow_buf_alloc, &overflow_buf)) {
      goto score_report_ret_NOMEM;
    }
    uintptr_t* missing_diploid_acc4 = &(missing_acc1[acc1_vec_ct * kWordsPerVec]);
    uintptr_t* missing_diploid_acc8 = &(missing_diploid_acc4[acc4_vec_ct * kWordsPerVec]);
    uintptr_t* missing_diploid_acc32 = &(missing_diploid_acc8[acc8_vec_ct * kWordsPerVec]);
    uintptr_t* missing_haploid_acc4 = &(missing_male_acc1[acc1_vec_ct * kWordsPerVec]);
    uintptr_t* missing_haploid_acc8 = &(missing_haploid_acc4[acc4_vec_ct * kWordsPerVec]);
    uintptr_t* missing_haploid_acc32 = &(missing_haploid_acc8[acc8_vec_ct * kWordsPerVec]);
    fill_ulong_zero(acc4_vec_ct * kWordsPerVec, missing_diploid_acc4);
    fill_ulong_zero(acc8_vec_ct * kWordsPerVec, missing_diploid_acc8);
    fill_ulong_zero(acc8_vec_ct * (4 * kWordsPerVec), missing_diploid_acc32);
    fill_ulong_zero(acc4_vec_ct * kWordsPerVec, missing_haploid_acc4);
    fill_ulong_zero(acc8_vec_ct * kWordsPerVec, missing_haploid_acc8);
    fill_ulong_zero(acc8_vec_ct * (4 * kWordsPerVec), missing_haploid_acc32);
    fill_cumulative_popcounts(sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
    copy_bitarr_subset(sex_male, sample_include, sample_ct, sex_nonmale_collapsed);
    bitarr_invert(sample_ct, sex_nonmale_collapsed);
    const uint32_t nonmale_ct = popcount_longs(sex_nonmale_collapsed, sample_ctl);
    const uint32_t male_ct = sample_ct - nonmale_ct;
    uint32_t* variant_id_htable = nullptr;
    uint32_t variant_id_htable_size;
    reterr = alloc_and_populate_id_htable_mt(variant_include, variant_ids, variant_ct, max_thread_ct, &variant_id_htable, nullptr, &variant_id_htable_size);
    if (reterr) {
      goto score_report_ret_1;
    }

    const uint32_t list_variants = (score_flags / kfScoreListVariants) & 1;
    if (list_variants) {
      const uint32_t output_zst = (score_flags / kfScoreListVariantsZs) & 1;
      outname_zst_set(".sscore.vars", output_zst, outname_end);
      reterr = cswrite_init(outname, 0, output_zst, max_thread_ct, overflow_buf_size, overflow_buf, R_CAST(unsigned char*, &(overflow_buf[overflow_buf_size])), &css);
      if (reterr) {
        goto score_report_ret_1;
      }
      cswritep = overflow_buf;
    }

    const int32_t x_code = cip->xymt_codes[kChrOffsetX];
    const int32_t y_code = cip->xymt_codes[kChrOffsetY];
    const int32_t mt_code = cip->xymt_codes[kChrOffsetMT];
    const uint32_t model_dominant = (score_flags / kfScoreDominant) & 1;
    const uint32_t domrec = model_dominant || (score_flags & kfScoreRecessive);
    const uint32_t variance_standardize = (score_flags / kfScoreVarianceStandardize) & 1;
    const uint32_t center = variance_standardize || (score_flags & kfScoreCenter);
    const uint32_t no_meanimpute = (score_flags / kfScoreNoMeanimpute) & 1;
    const uint32_t se_mode = (score_flags / kfScoreSe) & 1;
    uint32_t block_vidx = 0;
    uint32_t parity = 0;
    uint32_t cur_allele_ct = 2;
    double* cur_dosages_vmaj_iter = g_dosages_vmaj[0];
    double* cur_score_coefs_cmaj = g_score_coefs_cmaj[0];
    double geno_slope = kRecipDosageMax;
    double geno_intercept = 0.0;
    uint32_t variant_ct_rem15 = 15;
    uint32_t variant_ct_rem255d15 = 17;
    uint32_t variant_hap_ct_rem15 = 15;
    uint32_t variant_hap_ct_rem255d15 = 17;
    uint32_t allele_ct_base = 0;
    int32_t male_allele_ct_delta = 0;
    uint32_t valid_variant_ct = 0;
    uintptr_t missing_var_id_ct = 0;
    uintptr_t missing_allele_code_ct = 0;
#ifdef USE_MTBLAS
    const uint32_t matrix_multiply_thread_ct = (max_thread_ct > 1)? (max_thread_ct - 1) : 1;
    BLAS_SET_NUM_THREADS(matrix_multiply_thread_ct);
#endif
    pgr_clear_ld_cache(simple_pgrp);
    while (1) {
      if (!is_eoln_kns(*loadbuf_first_token)) {
        // varid_col_idx and allele_col_idx will almost always be very small
        char* variant_id_start = next_token_multz(loadbuf_first_token, varid_col_idx);
        if (!variant_id_start) {
          goto score_report_ret_MISSING_TOKENS;
        }
        char* variant_id_token_end = token_endnn(variant_id_start);
        const uint32_t variant_id_slen = variant_id_token_end - variant_id_start;
        uint32_t variant_uidx = variant_id_dupflag_htable_find(variant_id_start, variant_ids, variant_id_htable, variant_id_slen, variant_id_htable_size, max_variant_id_slen);
        if (!(variant_uidx >> 31)) {
          if (is_set(already_seen, variant_uidx)) {
            snprintf(g_logbuf, kLogbufSize, "Error: Variant ID '%s' appears multiple times in --score file.\n", variant_ids[variant_uidx]);
            goto score_report_ret_MALFORMED_INPUT_WW;
          }
          set_bit(variant_uidx, already_seen);
          char* allele_start = next_token_multz(loadbuf_first_token, allele_col_idx);
          if (!allele_start) {
            goto score_report_ret_MISSING_TOKENS;
          }
          uintptr_t variant_allele_idx_base;
          if (!variant_allele_idxs) {
            variant_allele_idx_base = variant_uidx * 2;
          } else {
            variant_allele_idx_base = variant_allele_idxs[variant_uidx];
            cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - variant_allele_idx_base;
          }
          char* allele_end = token_endnn(allele_start);
          char allele_end_char = *allele_end;
          *allele_end = '\0';
          const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
          uint32_t cur_allele_idx = 0;
          for (; cur_allele_idx < cur_allele_ct; ++cur_allele_idx) {
            // for very long alleles, tokequal_k might read past the end of the
            // workspace, so just use plain strcmp.
            if (!strcmp(allele_start, cur_alleles[cur_allele_idx])) {
              break;
            }
          }
          if (cur_allele_idx != cur_allele_ct) {
            // okay, the variant and allele are in our dataset.  Load it.
            // (todo: make this work in multiallelic case)
            uint32_t dosage_ct;
            uint32_t is_explicit_alt1;
            pglerr_t reterr = pgr_read_refalt1_genovec_dosage16_subset_unsafe(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec_buf, dosage_present_buf, dosage_vals_buf, &dosage_ct, &is_explicit_alt1);
            if (reterr) {
              if (reterr == kPglRetMalformedInput) {
                logprint("\n");
                logerrprint("Error: Malformed .pgen file.\n");
              }
              goto score_report_ret_1;
            }
            const uint32_t chr_idx = get_variant_chr(cip, variant_uidx);
            uint32_t is_relevant_x = (S_CAST(int32_t, chr_idx) == x_code);
            if ((domrec || variance_standardize) && (is_relevant_x || (S_CAST(int32_t, chr_idx) == mt_code))) {
              logerrprint("Error: --score 'dominant', 'recessive', and 'variance-standardize' modifiers\ncannot be used with chrX or MT.\n");
              goto score_report_ret_INCONSISTENT_INPUT;
            }
            const uint32_t is_nonx_haploid = (!is_relevant_x) && is_set(cip->haploid_mask, chr_idx);

            // only if --xchr-model 1 (which is no longer the default)
            is_relevant_x = is_relevant_x && xchr_model;

            const uint32_t is_y = (S_CAST(int32_t, chr_idx) == y_code);
            // pre-multiallelic kludge: current counts are for alt1, invert if
            // score is based on ref allele
            if (!cur_allele_idx) {
              genovec_invert_unsafe(sample_ct, genovec_buf);
              if (dosage_ct) {
                biallelic_dosage16_invert(dosage_ct, dosage_vals_buf);
              }
            }
            zero_trailing_quaters(sample_ct, genovec_buf);
            genovec_to_missingness_unsafe(genovec_buf, sample_ct, missing_acc1);
            if (dosage_ct) {
              bitvec_andnot(dosage_present_buf, sample_ctl, missing_acc1);
            }
            fill_cur_dosage_ints(genovec_buf, dosage_present_buf, dosage_vals_buf, sample_ct, dosage_ct, 2 - is_nonx_haploid, dosage_incrs);
            double ploidy_d;
            if (is_nonx_haploid) {
              if (is_y) {
                uint32_t sample_idx = 0;
                for (uint32_t nonmale_idx = 0; nonmale_idx < nonmale_ct; ++nonmale_idx, ++sample_idx) {
                  next_set_unsafe_ck(sex_nonmale_collapsed, &sample_idx);
                  dosage_incrs[sample_idx] = 0;
                }
                ++male_allele_ct_delta;
                bitvec_andnot(sex_nonmale_collapsed, sample_ctl, missing_acc1);
              } else {
                ++allele_ct_base;
              }
              unroll_incr_1_4(missing_acc1, acc1_vec_ct, missing_haploid_acc4);
              if (!(--variant_hap_ct_rem15)) {
                unroll_zero_incr_4_8(acc4_vec_ct, missing_haploid_acc4, missing_haploid_acc8);
                variant_hap_ct_rem15 = 15;
                if (!(--variant_hap_ct_rem255d15)) {
                  unroll_zero_incr_8_32(acc8_vec_ct, missing_haploid_acc8, missing_haploid_acc32);
                  variant_hap_ct_rem255d15 = 17;
                }
              }
              if (is_y) {
                memcpy(missing_male_acc1, missing_acc1, sample_ctl * sizeof(intptr_t));
                bitvec_or(sex_nonmale_collapsed, sample_ctl, missing_acc1);
              }
              ploidy_d = 1.0;
            } else {
              if (is_relevant_x) {
                uint32_t sample_idx = 0;
                for (uint32_t male_idx = 0; male_idx < male_ct; ++male_idx, ++sample_idx) {
                  next_unset_unsafe_ck(sex_nonmale_collapsed, &sample_idx);
                  dosage_incrs[sample_idx] /= 2;
                }
                bitvec_andnot_copy(missing_acc1, sex_nonmale_collapsed, sample_ctl, missing_male_acc1);
                bitvec_and(sex_nonmale_collapsed, sample_ctl, missing_acc1);
              }
              unroll_incr_1_4(missing_acc1, acc1_vec_ct, missing_diploid_acc4);
              if (!(--variant_ct_rem15)) {
                unroll_zero_incr_4_8(acc4_vec_ct, missing_diploid_acc4, missing_diploid_acc8);
                variant_ct_rem15 = 15;
                if (!(--variant_ct_rem255d15)) {
                  unroll_zero_incr_8_32(acc8_vec_ct, missing_diploid_acc8, missing_diploid_acc32);
                  variant_ct_rem255d15 = 17;
                }
              }
              allele_ct_base += 2;
              if (is_relevant_x) {
                --male_allele_ct_delta;
                unroll_incr_1_4(missing_male_acc1, acc1_vec_ct, missing_haploid_acc4);
                if (!(--variant_hap_ct_rem15)) {
                  unroll_zero_incr_4_8(acc4_vec_ct, missing_haploid_acc4, missing_haploid_acc8);
                  variant_hap_ct_rem15 = 15;
                  if (!(--variant_hap_ct_rem255d15)) {
                    unroll_zero_incr_8_32(acc8_vec_ct, missing_haploid_acc8, missing_haploid_acc32);
                    variant_hap_ct_rem255d15 = 17;
                  }
                }
                bitvec_or(missing_male_acc1, sample_ctl, missing_acc1);
              }
              if (!domrec) {
                ploidy_d = 2.0;
              } else {
                if (model_dominant) {
                  for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
                    if (dosage_incrs[sample_idx] > kDosageMax) {
                      dosage_incrs[sample_idx] = kDosageMax;
                    }
                  }
                } else {
                  for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
                    uint64_t cur_dosage_incr = dosage_incrs[sample_idx];
                    if (cur_dosage_incr <= kDosageMax) {
                      cur_dosage_incr = 0;
                    } else {
                      cur_dosage_incr -= kDosageMax;
                    }
                    dosage_incrs[sample_idx] = cur_dosage_incr;
                  }
                }
                ploidy_d = 1.0;
              }
            }
            for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
              dosage_sums[sample_idx] += dosage_incrs[sample_idx];
            }
            const double cur_allele_freq = get_allele_freq(&(allele_freqs[variant_allele_idx_base - variant_uidx]), cur_allele_idx, cur_allele_ct);
            if (center) {
              if (variance_standardize) {
                const double variance = ploidy_d * cur_allele_freq * (1.0 - cur_allele_freq);
                if (variance < kSmallEpsilon) {
                  zero_trailing_quaters(sample_ct, genovec_buf);
                  uint32_t genocounts[4];
                  genovec_count_freqs_unsafe(genovec_buf, sample_ct, genocounts);
                  if (dosage_ct || genocounts[1] || genocounts[2]) {
                    snprintf(g_logbuf, kLogbufSize, "Error: --score variance-standardize failure for ID '%s': estimated allele frequency is zero, but not all dosages are zero. (This is possible when e.g. allele frequencies are estimated from founders, but the allele is only observed in nonfounders.)\n", variant_ids[variant_uidx]);
                    goto score_report_ret_INCONSISTENT_INPUT_WW;
                  }
                  geno_slope = 0.0;
                } else {
                  geno_slope = kRecipDosageMax / sqrt(variance);
                }
              }
              // (ploidy * cur_allele_freq * kDosageMax) * geno_slope +
              //   geno_intercept == 0
              // bugfix: must use "-1.0 *" instead of - to avoid unsigned int
              //   wraparound
              geno_intercept = (-1.0 * kDosageMax) * ploidy_d * cur_allele_freq * geno_slope;
            }
            const uint32_t missing_ct = popcount_longs(missing_acc1, sample_ctl);
            const uint32_t nm_sample_ct = sample_ct - missing_ct;
            if (missing_ct) {
              double missing_effect = 0.0;
              if (!no_meanimpute) {
                missing_effect = kDosageMax * cur_allele_freq * geno_slope;
              }
              uint32_t sample_idx = 0;
              if (is_y || is_relevant_x) {
                fill_double_zero(sample_ct, cur_dosages_vmaj_iter);
                if (!no_meanimpute) {
                  const uint32_t male_missing_ct = popcount_longs(missing_male_acc1, sample_ctl);
                  for (uint32_t male_missing_idx = 0; male_missing_idx < male_missing_ct; ++male_missing_idx, ++sample_idx) {
                    next_set_unsafe_ck(missing_male_acc1, &sample_idx);
                    cur_dosages_vmaj_iter[sample_idx] = missing_effect;
                  }
                  if (is_relevant_x) {
                    // missing_male_acc1 not used after this point, so okay to
                    // use buffer for nonmales
                    bitvec_and_copy(missing_acc1, sex_nonmale_collapsed, sample_ctl, missing_male_acc1);
                    missing_effect *= 2;
                    const uint32_t nonmale_missing_ct = popcount_longs(missing_male_acc1, sample_ctl);
                    for (uint32_t nonmale_missing_idx = 0; nonmale_missing_idx < nonmale_missing_ct; ++nonmale_missing_idx, ++sample_idx) {
                      next_set_unsafe_ck(missing_male_acc1, &sample_idx);
                      cur_dosages_vmaj_iter[sample_idx] = missing_effect;
                    }
                  }
                }
              } else {
                missing_effect *= ploidy_d;
                for (uint32_t missing_idx = 0; missing_idx < missing_ct; ++missing_idx, ++sample_idx) {
                  next_set_unsafe_ck(missing_acc1, &sample_idx);
                  cur_dosages_vmaj_iter[sample_idx] = missing_effect;
                }
              }
            }
            uint32_t sample_idx = 0;
            for (uint32_t nm_sample_idx = 0; nm_sample_idx < nm_sample_ct; ++nm_sample_idx, ++sample_idx) {
              next_unset_unsafe_ck(missing_acc1, &sample_idx);
              cur_dosages_vmaj_iter[sample_idx] = u63tod(dosage_incrs[sample_idx]) * geno_slope + geno_intercept;
            }
            if (se_mode) {
              // Suppose our score coefficients are drawn from independent
              // Gaussians.  Then the variance of the final score average is
              // the sum of the variances of the individual terms, divided by
              // (T^2) where T is the number of terms.  These individual
              // variances are of the form ([genotype value] * [stdev])^2.
              //
              // Thus, we can use the same inner loop to compute standard
              // errors, as long as
              //   1. we square the genotypes and the standard errors before
              //      matrix multiplication, and
              //   2. we take the square root of the sums at the end.
              for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
                cur_dosages_vmaj_iter[sample_idx] *= cur_dosages_vmaj_iter[sample_idx];
              }
            }
            cur_dosages_vmaj_iter = &(cur_dosages_vmaj_iter[sample_ct]);

            *allele_end = allele_end_char;
            double* cur_score_coefs_iter = &(cur_score_coefs_cmaj[block_vidx]);
            const char* read_iter = loadbuf_first_token;
            for (uint32_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
              read_iter = next_token_multz(read_iter, score_col_idx_deltas[score_col_idx]);
              double raw_coef;
              const char* token_end = scanadv_double(read_iter, &raw_coef);
              if (!token_end) {
                snprintf(g_logbuf, kLogbufSize, "Error: Line %" PRIuPTR " of --score file has an invalid coefficient.\n", line_idx);
                goto score_report_ret_MALFORMED_INPUT_2;
              }
              *cur_score_coefs_iter = raw_coef;
              cur_score_coefs_iter = &(cur_score_coefs_iter[kScoreVariantBlockSize]);
              read_iter = token_end;
            }
            if (list_variants) {
              cswritep = strcpya(cswritep, variant_ids[variant_uidx]);
              append_binary_eoln(&cswritep);
              if (cswrite(&css, &cswritep)) {
                goto score_report_ret_WRITE_FAIL;
              }
            }
            ++valid_variant_ct;
            if (!(valid_variant_ct % 10000)) {
              printf("\r--score: %uk variants loaded.", valid_variant_ct / 1000);
              fflush(stdout);
            }
            ++block_vidx;
            if (block_vidx == kScoreVariantBlockSize) {
              if (se_mode) {
                for (uintptr_t ulii = 0; ulii < kScoreVariantBlockSize * score_col_ct; ++ulii) {
                  cur_score_coefs_cmaj[ulii] *= cur_score_coefs_cmaj[ulii];
                }
              }
              parity = 1 - parity;
              const uint32_t is_not_first_block = (ts.thread_func_ptr != nullptr);
              if (is_not_first_block) {
                join_threads3z(&ts);
              } else {
                ts.thread_func_ptr = calc_score_thread;
              }
              if (spawn_threads3z(is_not_first_block, &ts)) {
                goto score_report_ret_THREAD_CREATE_FAIL;
              }
              cur_dosages_vmaj_iter = g_dosages_vmaj[parity];
              cur_score_coefs_cmaj = g_score_coefs_cmaj[parity];
              block_vidx = 0;
            }
          } else {
            ++missing_allele_code_ct;
          }
        } else {
          if (variant_uidx != UINT32_MAX) {
            snprintf(g_logbuf, kLogbufSize, "Error: --score variant ID '%s' appears multiple times in main dataset.\n", variant_ids[variant_uidx & 0x7fffffff]);
            goto score_report_ret_INCONSISTENT_INPUT_WW;
          }
          ++missing_var_id_ct;
        }
      }
      if (!gzgets(gz_infile, loadbuf, loadbuf_size)) {
        if (!gzeof(gz_infile)) {
          goto score_report_ret_READ_FAIL;
        }
        break;
      }
      ++line_idx;
      if (!loadbuf[loadbuf_size - 1]) {
        goto score_report_ret_LONG_LINE;
      }
      loadbuf_first_token = skip_initial_spaces(loadbuf);
    }
    unroll_incr_4_8(missing_diploid_acc4, acc4_vec_ct, missing_diploid_acc8);
    unroll_incr_8_32(missing_diploid_acc8, acc8_vec_ct, missing_diploid_acc32);
    unroll_incr_4_8(missing_haploid_acc4, acc4_vec_ct, missing_haploid_acc8);
    unroll_incr_8_32(missing_haploid_acc8, acc8_vec_ct, missing_haploid_acc32);
    const uint32_t is_not_first_block = (ts.thread_func_ptr != nullptr);
    putc_unlocked('\r', stdout);
    if (missing_var_id_ct || missing_allele_code_ct) {
      if (!missing_var_id_ct) {
        snprintf(g_logbuf, kLogbufSize, "Warning: %" PRIuPTR " --score file entr%s.\n", missing_allele_code_ct, (missing_allele_code_ct == 1)? "y was skipped due to a mismatching allele code" : "ies were skipped due to mismatching allele codes");
      } else if (!missing_allele_code_ct) {
        snprintf(g_logbuf, kLogbufSize, "Warning: %" PRIuPTR " --score file entr%s.\n", missing_var_id_ct, (missing_var_id_ct == 1)? "y was skipped due to a missing variant ID" : "ies were skipped due to missing variant IDs");
      } else {
        snprintf(g_logbuf, kLogbufSize, "Warning: %" PRIuPTR " --score file entr%s, and %" PRIuPTR " %s.\n", missing_var_id_ct, (missing_var_id_ct == 1)? "y was skipped due to a missing variant ID" : "ies were skipped due to missing variant IDs", missing_allele_code_ct, (missing_allele_code_ct == 1)? "was skipped due to a mismatching allele code" : "were skipped due to mismatching allele codes");
      }
      wordwrapb(0);
      logerrprintb();
      if (!list_variants) {
        logerrprint("(Add the 'list-variants' modifier to see which variants were actually used for\nscoring.)\n");
      }
    }
    if (block_vidx) {
      if (is_not_first_block) {
        join_threads3z(&ts);
      } else {
        ts.thread_func_ptr = calc_score_thread;
      }
    } else if (!valid_variant_ct) {
      logerrprint("Error: No valid variants in --score file.\n");
      goto score_report_ret_MALFORMED_INPUT;
    } else {
      join_threads3z(&ts);
    }
    ts.is_last_block = 1;
    g_cur_batch_size = block_vidx;
    if (se_mode) {
      for (uintptr_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
        double* cur_score_coefs_row = &(cur_score_coefs_cmaj[score_col_idx * kScoreVariantBlockSize]);
        for (uint32_t uii = 0; uii < block_vidx; ++uii) {
          cur_score_coefs_row[uii] *= cur_score_coefs_row[uii];
        }
      }
    }
    if (spawn_threads3z(is_not_first_block, &ts)) {
      goto score_report_ret_THREAD_CREATE_FAIL;
    }
    join_threads3z(&ts);
    if (gzclose_null(&gz_infile)) {
      goto score_report_ret_READ_FAIL;
    }
    if (se_mode) {
      // sample_ct * score_col_ct
      for (uintptr_t ulii = 0; ulii < sample_ct * score_col_ct; ++ulii) {
        g_final_scores_cmaj[ulii] = sqrt(g_final_scores_cmaj[ulii]);
      }
    }
    LOGPRINTF("--score: %u variant%s processed.\n", valid_variant_ct, (valid_variant_ct == 1)? "" : "s");
    if (list_variants) {
      if (cswrite_close_null(&css, cswritep)) {
        goto score_report_ret_WRITE_FAIL;
      }
      cswritep = nullptr;
      LOGPRINTF("Variant list written to %s .\n", outname);
    }

    const uint32_t output_zst = (score_flags / kfScoreZs) & 1;
    outname_zst_set(".sscore", output_zst, outname_end);
    reterr = cswrite_init(outname, 0, output_zst, max_thread_ct, overflow_buf_size, overflow_buf, R_CAST(unsigned char*, &(overflow_buf[overflow_buf_size])), &css);
    if (reterr) {
      goto score_report_ret_1;
    }
    cswritep = overflow_buf;
    // see e.g. write_psam() in plink2_data.cc
    const uint32_t write_sid = sid_col_required(sample_include, sids, sample_ct, max_sid_blen, score_flags / kfScoreColMaybesid);
    const uint32_t write_empty_pheno = (score_flags & kfScoreColPheno1) && (!pheno_ct);
    const uint32_t write_phenos = (score_flags & (kfScoreColPheno1 | kfScoreColPhenos)) && pheno_ct;
    if (write_phenos && (!(score_flags & kfScoreColPhenos))) {
      pheno_ct = 1;
    }
    cswritep = strcpya(cswritep, "#FID\tIID");
    if (write_sid) {
      cswritep = strcpya(cswritep, "\tSID");
    }
    if (write_phenos) {
      for (uint32_t pheno_idx = 0; pheno_idx < pheno_ct; ++pheno_idx) {
        *cswritep++ = '\t';
        cswritep = strcpya(cswritep, &(pheno_names[pheno_idx * max_pheno_name_blen]));
        if (cswrite(&css, &cswritep)) {
          goto score_report_ret_WRITE_FAIL;
        }
      }
    } else if (write_empty_pheno) {
      cswritep = strcpya(cswritep, "\tPHENO1");
    }
    const uint32_t write_nmiss_allele = (score_flags / kfScoreColNmissAllele) & 1;
    if (write_nmiss_allele) {
      cswritep = strcpya(cswritep, "\tNMISS_ALLELE_CT");
    }
    const uint32_t write_denom = (score_flags / kfScoreColDenom) & 1;
    if (write_denom) {
      cswritep = strcpya(cswritep, "\tDENOM");
    }
    const uint32_t write_dosage_sum = (score_flags / kfScoreColDosageSum) & 1;
    if (write_dosage_sum) {
      cswritep = strcpya(cswritep, "\tNAMED_ALLELE_DOSAGE_SUM");
    }
    if (write_score_avgs) {
      for (uint32_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
        *cswritep++ = '\t';
        cswritep = strcpya(cswritep, score_col_names[score_col_idx]);
        cswritep = strcpya(cswritep, "_AVG");
        if (cswrite(&css, &cswritep)) {
          goto score_report_ret_WRITE_FAIL;
        }
      }
    }
    if (write_score_sums) {
      for (uint32_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
        *cswritep++ = '\t';
        cswritep = strcpya(cswritep, score_col_names[score_col_idx]);
        cswritep = strcpya(cswritep, "_SUM");
        if (cswrite(&css, &cswritep)) {
          goto score_report_ret_WRITE_FAIL;
        }
      }
    }
    append_binary_eoln(&cswritep);
    const uint32_t* scrambled_missing_diploid_cts = R_CAST(uint32_t*, missing_diploid_acc32);
    const uint32_t* scrambled_missing_haploid_cts = R_CAST(uint32_t*, missing_haploid_acc32);
    const char* output_missing_pheno = g_output_missing_pheno;
    const uint32_t omp_slen = strlen(output_missing_pheno);

    uint32_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
      next_set_unsafe_ck(sample_include, &sample_uidx);
      cswritep = strcpya(cswritep, &(sample_ids[sample_uidx * max_sample_id_blen]));
      if (write_sid) {
        *cswritep++ = '\t';
        if (sids) {
          cswritep = strcpya(cswritep, &(sids[max_sid_blen * sample_uidx]));
        } else {
          *cswritep++ = '0';
        }
      }
      if (write_phenos) {
        // er, this probably belongs in its own function
        for (uint32_t pheno_idx = 0; pheno_idx < pheno_ct; ++pheno_idx) {
          const pheno_col_t* cur_pheno_col = &(pheno_cols[pheno_idx]);
          const pheno_dtype_t type_code = cur_pheno_col->type_code;
          *cswritep++ = '\t';
          if (type_code <= kPhenoDtypeQt) {
            if (!IS_SET(cur_pheno_col->nonmiss, sample_uidx)) {
              cswritep = memcpya(cswritep, output_missing_pheno, omp_slen);
            } else if (type_code == kPhenoDtypeCc) {
              *cswritep++ = '1' + IS_SET(cur_pheno_col->data.cc, sample_uidx);
            } else {
              cswritep = dtoa_g(cur_pheno_col->data.qt[sample_uidx], cswritep);
            }
          } else {
            // category index guaranteed to be zero for missing values
            cswritep = strcpya(cswritep, cur_pheno_col->category_names[cur_pheno_col->data.cat[sample_uidx]]);
            if (cswrite(&css, &cswritep)) {
              goto score_report_ret_WRITE_FAIL;
            }
          }
        }
      } else if (write_empty_pheno) {
        *cswritep++ = '\t';
        cswritep = memcpya(cswritep, output_missing_pheno, omp_slen);
      }
      const uint32_t scrambled_idx = scramble_1_4_8_32(sample_idx);
      uint32_t denom = allele_ct_base + is_set(sex_male, sample_uidx) * male_allele_ct_delta;
      const uint32_t nmiss_allele_ct = denom - 2 * scrambled_missing_diploid_cts[scrambled_idx] - scrambled_missing_haploid_cts[scrambled_idx];
      if (write_nmiss_allele) {
        *cswritep++ = '\t';
        cswritep = uint32toa(nmiss_allele_ct, cswritep);
      }
      if (no_meanimpute) {
        denom = nmiss_allele_ct;
      }
      if (write_denom) {
        *cswritep++ = '\t';
        cswritep = uint32toa(denom, cswritep);
      }
      if (write_dosage_sum) {
        *cswritep++ = '\t';
        cswritep = print_dosage(dosage_sums[sample_idx], cswritep);
      }
      const double* final_score_col = &(g_final_scores_cmaj[sample_idx]);
      if (write_score_avgs) {
        const double denom_recip = 1.0 / S_CAST(double, denom);
        for (uint32_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
          *cswritep++ = '\t';
          cswritep = dtoa_g(final_score_col[score_col_idx * sample_ct] * denom_recip, cswritep);
        }
      }
      if (write_score_sums) {
        for (uint32_t score_col_idx = 0; score_col_idx < score_col_ct; ++score_col_idx) {
          *cswritep++ = '\t';
          cswritep = dtoa_g(final_score_col[score_col_idx * sample_ct], cswritep);
        }
      }
      append_binary_eoln(&cswritep);
      if (cswrite(&css, &cswritep)) {
        goto score_report_ret_WRITE_FAIL;
      }
    }
    if (cswrite_close_null(&css, cswritep)) {
      goto score_report_ret_WRITE_FAIL;
    }
    LOGPRINTFWW("--score: Results written to %s .\n", outname);
  }
  while (0) {
  score_report_ret_LONG_LINE:
    if (loadbuf_size == kMaxLongLine) {
      LOGERRPRINTF("Error: Line %" PRIuPTR " of --score file is pathologically long.\n", line_idx);
      reterr = kPglRetMalformedInput;
      break;
    }
  score_report_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  score_report_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  score_report_ret_WRITE_FAIL:
    reterr = kPglRetReadFail;
    break;
  score_report_ret_INVALID_CMDLINE:
    reterr = kPglRetInvalidCmdline;
    break;
  score_report_ret_MALFORMED_INPUT_WW:
    wordwrapb(0);
  score_report_ret_MALFORMED_INPUT_2:
    logprint("\n");
    logerrprintb();
  score_report_ret_MALFORMED_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  score_report_ret_MISSING_TOKENS:
    logprint("\n");
    LOGERRPRINTFWW("Error: Line %" PRIuPTR " of %s has fewer tokens than expected.\n", line_idx, score_info_ptr->input_fname);
    reterr = kPglRetInconsistentInput;
    break;
  score_report_ret_INCONSISTENT_INPUT_WW:
    wordwrapb(0);
    logprint("\n");
    logerrprintb();
  score_report_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  score_report_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 score_report_ret_1:
  cswrite_close_cond(&css, cswritep);
  threads3z_cleanup(&ts, &g_cur_batch_size);
  BLAS_SET_NUM_THREADS(1);
  gzclose_cond(gz_infile);
  bigstack_double_reset(bigstack_mark, bigstack_end_mark);
  return reterr;
}

#ifdef __cplusplus
} // namespace plink2
#endif