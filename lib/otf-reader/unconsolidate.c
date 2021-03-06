#include "unconsolidate.h"
#include "support/util.h"
#include "support/aglfn/aglfn.h"
#include "support/sha1/sha1.h"

typedef struct { uint8_t hash[SHA1_BLOCK_SIZE]; } GlyphHash;

GlyphHash nameGlyphByHash(glyf_Glyph *g, table_glyf *glyf) {
	caryll_Buffer *buf = bufnew();
	bufwrite8(buf, 'H');
	bufwrite32b(buf, g->advanceWidth);
	bufwrite8(buf, 'V');
	bufwrite32b(buf, g->advanceHeight);
	bufwrite8(buf, 'v');
	bufwrite32b(buf, g->verticalOrigin);
	// contours
	bufwrite8(buf, 'C');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->contours.length; j++) {
		bufwrite8(buf, '(');
		glyf_Contour *c = &g->contours.items[j];
		for (shapeid_t k = 0; k < c->length; k++) {
			bufwrite32b(buf, otfcc_to_fixed(c->items[k].x));
			bufwrite32b(buf, otfcc_to_fixed(c->items[k].y));
			bufwrite8(buf, c->items[k].onCurve ? 1 : 0);
		}
		bufwrite8(buf, ')');
	}
	bufwrite8(buf, ')');
	// references
	bufwrite8(buf, 'R');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->references.length; j++) {
		glyf_ComponentReference *r = &g->references.items[j];
		GlyphHash h = nameGlyphByHash(glyf->items[r->glyph.index], glyf);
		bufwrite_bytes(buf, SHA1_BLOCK_SIZE, h.hash);
		bufwrite32b(buf, otfcc_to_fixed(r->x));
		bufwrite32b(buf, otfcc_to_fixed(r->y));
		bufwrite32b(buf, otfcc_to_f2dot14(r->a));
		bufwrite32b(buf, otfcc_to_f2dot14(r->b));
		bufwrite32b(buf, otfcc_to_f2dot14(r->c));
		bufwrite32b(buf, otfcc_to_f2dot14(r->d));
	}
	bufwrite8(buf, ')');
	// stemH, stemV
	bufwrite8(buf, 's'), bufwrite8(buf, 'H');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->stemH.length; j++) {
		bufwrite32b(buf, otfcc_to_fixed(g->stemH.items[j].position));
		bufwrite32b(buf, otfcc_to_fixed(g->stemH.items[j].width));
	}
	bufwrite8(buf, ')');
	bufwrite8(buf, 's'), bufwrite8(buf, 'V');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->stemV.length; j++) {
		bufwrite32b(buf, otfcc_to_fixed(g->stemV.items[j].position));
		bufwrite32b(buf, otfcc_to_fixed(g->stemV.items[j].width));
	}
	bufwrite8(buf, ')');
	// hintmask, contourmask
	bufwrite8(buf, 'm'), bufwrite8(buf, 'H');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->hintMasks.length; j++) {
		bufwrite16b(buf, g->hintMasks.items[j].contoursBefore);
		bufwrite16b(buf, g->hintMasks.items[j].pointsBefore);
		for (shapeid_t k = 0; k < g->stemH.length; k++) {
			bufwrite8(buf, g->hintMasks.items[j].maskH[k]);
		}
		for (shapeid_t k = 0; k < g->stemV.length; k++) {
			bufwrite8(buf, g->hintMasks.items[j].maskV[k]);
		}
	}
	bufwrite8(buf, ')');
	bufwrite8(buf, 'm'), bufwrite8(buf, 'C');
	bufwrite8(buf, '(');
	for (shapeid_t j = 0; j < g->contourMasks.length; j++) {
		bufwrite16b(buf, g->contourMasks.items[j].contoursBefore);
		bufwrite16b(buf, g->contourMasks.items[j].pointsBefore);
		for (shapeid_t k = 0; k < g->stemH.length; k++) {
			bufwrite8(buf, g->contourMasks.items[j].maskH[k]);
		}
		for (shapeid_t k = 0; k < g->stemV.length; k++) {
			bufwrite8(buf, g->contourMasks.items[j].maskV[k]);
		}
	}
	bufwrite8(buf, ')');
	// instructions
	bufwrite8(buf, 'I');
	bufwrite32b(buf, g->instructionsLength);
	bufwrite_bytes(buf, g->instructionsLength, g->instructions);
	// Generate SHA1
	SHA1_CTX ctx;
	uint8_t hash[SHA1_BLOCK_SIZE];
	sha1_init(&ctx);
	sha1_update(&ctx, buf->data, buflen(buf));
	sha1_final(&ctx, hash);
	GlyphHash h;
	for (uint16_t j = 0; j < SHA1_BLOCK_SIZE; j++) {
		h.hash[j] = hash[j];
	}
	buffree(buf);
	return h;
}

// Unconsolidation: Remove redundent data and de-couple internal data
// It does these things:
//   1. Merge hmtx data into glyf
//   2. Replace all glyph IDs into glyph names. Note all glyph references with
//      same name whare one unique string entity stored in font->glyph_order.
//      (Separate?)
static otfcc_GlyphOrder *createGlyphOrder(otfcc_Font *font, const otfcc_Options *options) {
	otfcc_GlyphOrder *glyph_order = GlyphOrder.create();
	otfcc_GlyphOrder *aglfn = GlyphOrder.create();
	aglfn_setupNames(aglfn);
	glyphid_t numGlyphs = font->glyf->length;
	sds prefix;
	if (options->glyph_name_prefix) {
		prefix = sdsnew(options->glyph_name_prefix);
	} else {
		prefix = sdsempty();
	}

	// pass 1: Map to existing glyph names
	for (glyphid_t j = 0; j < numGlyphs; j++) {
		glyf_Glyph *g = font->glyf->items[j];
		if (options->name_glyphs_by_hash) { // name by hash
			GlyphHash h = nameGlyphByHash(g, font->glyf);
			sds gname = sdsempty();
			for (uint16_t j = 0; j < SHA1_BLOCK_SIZE; j++) {
				if (!(j % 4) && (j / 4)) {
					gname = sdscatprintf(gname, "-%02X", h.hash[j]);
				} else {
					gname = sdscatprintf(gname, "%02X", h.hash[j]);
				}
			}
			if (GlyphOrder.lookupName(glyph_order, gname)) {
				// found duplicate glyph
				glyphid_t n = 2;
				bool stillIn = false;
				do {
					if (stillIn) n += 1;
					sds newname = sdscatprintf(sdsempty(), "%s-%s%d", gname, prefix, n);
					stillIn = GlyphOrder.lookupName(glyph_order, newname);
					sdsfree(newname);
				} while (stillIn);
				sds newname = sdscatprintf(sdsempty(), "%s-%s%d", gname, prefix, n);
				sds sharedName = GlyphOrder.setByGID(glyph_order, j, newname);
				if (g->name) sdsfree(g->name);
				g->name = sdsdup(sharedName);
				sdsfree(gname);
			} else {
				sds sharedName = GlyphOrder.setByGID(glyph_order, j, gname);
				if (g->name) sdsfree(g->name);
				g->name = sdsdup(sharedName);
			}
		} else if (options->ignore_glyph_order) { // ignore built-in names
			                                      // pass
		} else if (g->name) {
			sds gname = sdscatprintf(sdsempty(), "%s%s", prefix, g->name);
			sds sharedName = GlyphOrder.setByGID(glyph_order, j, gname);
			if (g->name) sdsfree(g->name);
			g->name = sdsdup(sharedName);
		}
	}

	// pass 2: Map to `post` names
	if (font->post != NULL && font->post->post_name_map != NULL && !options->ignore_glyph_order) {
		otfcc_GlyphOrderEntry *s, *tmp;
		HASH_ITER(hhID, font->post->post_name_map->byGID, s, tmp) {
			sds gname = sdscatprintf(sdsempty(), "%s%s", prefix, s->name);
			GlyphOrder.setByGID(glyph_order, s->gid, gname);
		}
	}

	// pass 3: Map to AGLFN & Unicode
	if (font->cmap != NULL) {
		cmap_Entry *s;
		foreach_hash(s, font->cmap->unicodes) if (s->glyph.index > 0) {
			sds name = NULL;
			GlyphOrder.nameAField_Shared(aglfn, s->unicode, &name);
			if (name == NULL) {
				name = sdscatprintf(sdsempty(), "%suni%04X", prefix, s->unicode);
			} else {
				name = sdscatprintf(sdsempty(), "%s%s", prefix, name);
			}
			GlyphOrder.setByGID(glyph_order, s->glyph.index, name);
		}
	}

	// pass 4 : Map to GID
	for (glyphid_t j = 0; j < numGlyphs; j++) {
		sds name;
		if (j > 1) {
			name = sdscatfmt(sdsempty(), "%sglyph%u", prefix, j);
		} else if (j == 1) {
			// GID 1 may often be ".null"
			if (font->glyf->items[1] && !font->glyf->items[1]->contours.length &&
			    !font->glyf->items[1]->references.length) {
				name = sdsnew(".null");
			} else {
				name = sdscatfmt(sdsempty(), "%sglyph%u", prefix, j);
			}
		} else {
			name = sdscatfmt(sdsempty(), "%s.notdef", prefix);
		}
		GlyphOrder.setByGID(glyph_order, j, name);
	}

	GlyphOrder.free(aglfn);
	sdsfree(prefix);
	return glyph_order;
}

static void nameGlyphs(otfcc_Font *font, otfcc_GlyphOrder *gord) {
	if (!gord) return;
	for (glyphid_t j = 0; j < font->glyf->length; j++) {
		glyf_Glyph *g = font->glyf->items[j];
		sds glyphName = NULL;
		GlyphOrder.nameAField_Shared(gord, j, &glyphName);
		if (g->name) sdsfree(g->name);
		g->name = sdsdup(glyphName);
	}
}

static void unconsolidate_chaining(otfcc_Font *font, otl_Lookup *lookup, table_OTL *table) {
	tableid_t totalRules = 0;
	for (tableid_t j = 0; j < lookup->subtables.length; j++) {
		if (!lookup->subtables.items[j]) continue;
		if (lookup->subtables.items[j]->chaining.type == otl_chaining_poly) {
			totalRules += lookup->subtables.items[j]->chaining.rulesCount;
		} else if (lookup->subtables.items[j]->chaining.type == otl_chaining_canonical) {
			totalRules += 1;
		}
	}
	otl_SubtableList newsts;
	otl_iSubtableList.init(&newsts);
	for (tableid_t j = 0; j < lookup->subtables.length; j++) {
		if (!lookup->subtables.items[j]) continue;
		if (lookup->subtables.items[j]->chaining.type == otl_chaining_poly) {
			for (tableid_t k = 0; k < lookup->subtables.items[j]->chaining.rulesCount; k++) {
				otl_Subtable *st;
				NEW(st);
				st->chaining.type = otl_chaining_canonical;
				// transfer ownership of rule
				st->chaining.rule = *(lookup->subtables.items[j]->chaining.rules[k]);
				FREE(lookup->subtables.items[j]->chaining.rules[k]);
				otl_iSubtableList.push(&newsts, st);
			}
			FREE(lookup->subtables.items[j]->chaining.rules);
			FREE(lookup->subtables.items[j]);
		} else if (lookup->subtables.items[j]->chaining.type == otl_chaining_canonical) {
			otl_Subtable *st;
			NEW(st);
			st->chaining.type = otl_chaining_canonical;
			st->chaining.rule = lookup->subtables.items[j]->chaining.rule;
			otl_iSubtableList.push(&newsts, st);
			lookup->subtables.items[j] = NULL;
		}
	}
	otl_iSubtableList.disposeDependent(&lookup->subtables, lookup);
	lookup->subtables = newsts;
}

static void expandChain(otfcc_Font *font, otl_Lookup *lookup, table_OTL *table) {
	switch (lookup->type) {
		case otl_type_gsub_chaining:
		case otl_type_gpos_chaining:
			unconsolidate_chaining(font, lookup, table);
			break;
		default:
			break;
	}
}

static void expandChainingLookups(otfcc_Font *font) {
	if (font->GSUB) {
		for (uint32_t j = 0; j < font->GSUB->lookups.length; j++) {
			otl_Lookup *lookup = font->GSUB->lookups.items[j];
			expandChain(font, lookup, font->GSUB);
		}
	}
	if (font->GPOS) {
		for (uint32_t j = 0; j < font->GPOS->lookups.length; j++) {
			otl_Lookup *lookup = font->GPOS->lookups.items[j];
			expandChain(font, lookup, font->GPOS);
		}
	}
}

static void mergeHmtx(otfcc_Font *font) {
	// Merge hmtx table into glyf.
	if (font->hhea && font->hmtx && font->glyf) {
		uint32_t count_a = font->hhea->numberOfMetrics;
		for (glyphid_t j = 0; j < font->glyf->length; j++) {
			font->glyf->items[j]->advanceWidth = font->hmtx->metrics[(j < count_a ? j : count_a - 1)].advanceWidth;
		}
	}
}

static void mergeVmtx(otfcc_Font *font) {
	// Merge vmtx table into glyf.
	if (font->vhea && font->vmtx && font->glyf) {
		uint32_t count_a = font->vhea->numOfLongVerMetrics;
		for (glyphid_t j = 0; j < font->glyf->length; j++) {
			font->glyf->items[j]->advanceHeight = font->vmtx->metrics[(j < count_a ? j : count_a - 1)].advanceHeight;
			if (j < count_a) {
				font->glyf->items[j]->verticalOrigin = font->vmtx->metrics[j].tsb + font->glyf->items[j]->stat.yMax;
			} else {
				font->glyf->items[j]->verticalOrigin =
				    font->vmtx->topSideBearing[j - count_a] + font->glyf->items[j]->stat.yMax;
			}
		}
		if (font->VORG) {
			for (glyphid_t j = 0; j < font->glyf->length; j++) {
				font->glyf->items[j]->verticalOrigin = font->VORG->defaultVerticalOrigin;
			}
			for (glyphid_t j = 0; j < font->VORG->numVertOriginYMetrics; j++) {
				if (font->VORG->entries[j].gid < font->glyf->length) {
					font->glyf->items[font->VORG->entries[j].gid]->verticalOrigin =
					    font->VORG->entries[j].verticalOrigin;
				}
			}
		}
	}
}
static void mergeLTSH(otfcc_Font *font) {
	if (font->glyf && font->LTSH) {
		for (glyphid_t j = 0; j < font->glyf->length && j < font->LTSH->numGlyphs; j++) {
			font->glyf->items[j]->yPel = font->LTSH->yPels[j];
		}
	}
}
void otfcc_unconsolidateFont(otfcc_Font *font, const otfcc_Options *options) {
	// Merge metrics
	mergeHmtx(font);
	mergeVmtx(font);
	mergeLTSH(font);
	// expand chaining lookups
	expandChainingLookups(font);
	// Name glyphs
	if (font->glyf) {
		otfcc_GlyphOrder *gord = createGlyphOrder(font, options);
		nameGlyphs(font, gord);
		GlyphOrder.free(gord);
	}
}
