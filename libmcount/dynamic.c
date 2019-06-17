#include <string.h>
#include <stdint.h>
#include <link.h>
#include <sys/mman.h>

/* This should be defined before #include "utils.h" */
#define PR_FMT     "dynamic"
#define PR_DOMAIN  DBG_DYNAMIC

#include "libmcount/mcount.h"
#include "libmcount/internal.h"
#include "utils/utils.h"
#include "utils/symbol.h"
#include "utils/filter.h"
#include "utils/rbtree.h"
#include "utils/list.h"

static struct mcount_dynamic_info *mdinfo;
static struct mcount_dynamic_stats {
	int total;
	int failed;
	int skipped;
	int nomatch;
} stats;

#define PAGE_SIZE   4096
#define CODE_CHUNK  (PAGE_SIZE * 8)

struct code_page {
	struct list_head	list;
	void			*page;
	int			pos;
};

static LIST_HEAD(code_pages);
static struct rb_root code_tree = RB_ROOT;

static struct mcount_orig_insn *lookup_code(struct rb_root *root,
					    unsigned long addr, bool create)
{
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;
	struct mcount_orig_insn *iter;

	while (*p) {
		parent = *p;
		iter = rb_entry(parent, struct mcount_orig_insn, node);

		if (iter->addr == addr)
			return iter;

		if (iter->addr > addr)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	if (!create)
		return NULL;

	iter = xmalloc(sizeof(*iter));
	iter->addr = addr;

	rb_link_node(&iter->node, parent, p);
	rb_insert_color(&iter->node, root);
	return iter;
}

struct mcount_orig_insn *mcount_save_code(unsigned long addr, unsigned insn_size,
					  void *jmp_insn, unsigned jmp_size)
{
	struct code_page *cp = NULL;
	struct mcount_orig_insn *orig;
	const int patch_size = ALIGN(insn_size + jmp_size, 32);

	if (!list_empty(&code_pages))
		cp = list_last_entry(&code_pages, struct code_page, list);

	if (cp == NULL || (cp->pos + patch_size > CODE_CHUNK)) {
		cp = xmalloc(sizeof(*cp));
		cp->page = mmap(NULL, CODE_CHUNK, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (cp->page == MAP_FAILED)
			pr_err("mmap code page failed");
		cp->pos = 0;

		list_add_tail(&cp->list, &code_pages);
	}

	orig = lookup_code(&code_tree, addr, true);
	orig->insn = cp->page + cp->pos;

	memcpy(orig->insn, (void *)addr, insn_size);
	memcpy(orig->insn + insn_size, jmp_insn, jmp_size);

	cp->pos += patch_size;
	return orig;
}

void mcount_freeze_code(void)
{
	struct code_page *cp;

	list_for_each_entry(cp, &code_pages, list)
		mprotect(cp->page, CODE_CHUNK, PROT_EXEC);
}

void *mcount_find_code(unsigned long addr)
{
	struct mcount_orig_insn *orig;

	orig = lookup_code(&code_tree, addr, false);
	if (orig == NULL)
		pr_err_ns("cannot find matching patch insn: %#lx\n", addr);

	return orig->insn;
}

/* dummy functions (will be overridden by arch-specific code) */
__weak int mcount_setup_trampoline(struct mcount_dynamic_info *mdi)
{
	return -1;
}

__weak void mcount_cleanup_trampoline(struct mcount_dynamic_info *mdi)
{
}

__weak int mcount_patch_func(struct mcount_dynamic_info *mdi, struct sym *sym,
			     struct mcount_disasm_engine *disasm)
{
	return -1;
}

__weak void mcount_arch_find_module(struct mcount_dynamic_info *mdi,
				    struct symtab *symtab)
{
	mdi->arch = NULL;
}

__weak void mcount_disasm_init(struct mcount_disasm_engine *disasm)
{
}

__weak void mcount_disasm_finish(struct mcount_disasm_engine *disasm)
{
}

/* callback for dl_iterate_phdr() */
static int find_dynamic_module(struct dl_phdr_info *info, size_t sz, void *data)
{
	const char *name = info->dlpi_name;
	struct mcount_dynamic_info *mdi;
	struct symtabs *symtabs = data;
	struct uftrace_mmap *map;
	bool base_addr_set = false;
	unsigned i;

	mdi = xzalloc(sizeof(*mdi));

	if (name[0] == '\0')
		mdi->mod_name = xstrdup(read_exename());
	else
		mdi->mod_name = xstrdup(name);

	for (i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type != PT_LOAD)
			continue;

		if (!base_addr_set) {
			mdi->base_addr = info->dlpi_phdr[i].p_vaddr;
			base_addr_set = true;
		}

		if (!(info->dlpi_phdr[i].p_flags & PF_X))
			continue;

		/* find address and size of code segment */
		mdi->text_addr = info->dlpi_phdr[i].p_vaddr;
		mdi->text_size = info->dlpi_phdr[i].p_memsz;
		break;
	}
	mdi->base_addr += info->dlpi_addr;
	mdi->text_addr += info->dlpi_addr;

	map = find_map(symtabs, mdi->base_addr);
	if (map && map->mod) {
		mdi->sym_base = map->start;
		mdi->nr_symbols = map->mod->symtab.nr_sym;
		mcount_arch_find_module(mdi, &map->mod->symtab);
		mdi->next = mdinfo;
		mdinfo = mdi;
	}

	return 0;
}

static void prepare_dynamic_update(struct mcount_disasm_engine *disasm,
				  struct symtabs *symtabs)
{
	mcount_disasm_init(disasm);
	dl_iterate_phdr(find_dynamic_module, symtabs);
}

struct mcount_dynamic_info *setup_trampoline(struct uftrace_mmap *map)
{
	struct mcount_dynamic_info *mdi;

	for (mdi = mdinfo; mdi != NULL; mdi = mdi->next) {
		if (map->start == mdi->sym_base)
			break;
	}

	if (mdi != NULL && mdi->trampoline == 0) {
		if (mcount_setup_trampoline(mdi) < 0)
			mdi = NULL;
	}

	return mdi;
}

static int do_dynamic_update(struct symtabs *symtabs, char *patch_funcs,
			     enum uftrace_pattern_type ptype,
			     struct mcount_disasm_engine *disasm)
{
	struct symtab *symtab;
	struct strv funcs = STRV_INIT;
	char *name;
	int j;
	/* skip special startup (csu) functions */
	const char *csu_skip_syms[] = {
		"_start",
		"__libc_csu_init",
		"__libc_csu_fini",
	};

	if (patch_funcs == NULL)
		return 0;

	strv_split(&funcs, patch_funcs, ";");

	strv_for_each(&funcs, name, j) {
		bool found = false;
		bool csu_skip;
		char *modname, *delim;
		unsigned i, k;
		struct sym *sym;
		struct uftrace_pattern patt;
		struct uftrace_mmap *map;
		struct mcount_dynamic_info *mdi = NULL;

		delim = strchr(name, '@');

		if (delim == NULL) {
			/* first of uftrace_mmap is main module always. */
			map = symtabs->maps;
			symtab = &symtabs->maps->mod->symtab;
		}
		else {
			*delim = '\0';
			modname = ++delim;
			map = find_map_by_name(symtabs, modname);
			if (map == NULL || map->mod == NULL) {
				pr_dbg("Failed to find map of %s\n", modname);
				continue;
			}
			symtab = &map->mod->symtab;
		}

		mdi = setup_trampoline(map);
		if (mdi == NULL) {
			pr_warn("Failed to set the trampoline into %s\n", map->libname);
			continue;
		}

		init_filter_pattern(ptype, &patt, name);

		for (i = 0; i < symtab->nr_sym; i++) {
			sym = &symtab->sym[i];

			csu_skip = false;
			for (k = 0; k < ARRAY_SIZE(csu_skip_syms); k++) {
				if (!strcmp(sym->name, csu_skip_syms[k])) {
					csu_skip = true;
					break;
				}
			}
			if (csu_skip)
				continue;

			if (sym->type != ST_LOCAL_FUNC &&
			    sym->type != ST_GLOBAL_FUNC)
				continue;

			if (!match_filter_pattern(&patt, sym->name))
				continue;

			found = true;
			switch (mcount_patch_func(mdi, sym, disasm)) {
			case INSTRUMENT_FAILED:
				stats.failed++;
				break;
			case INSTRUMENT_SKIPPED:
				stats.skipped++;
				break;
			case INSTRUMENT_SUCCESS:
			default:
				break;
			}
			stats.total++;
		}

		if (!found)
			stats.nomatch++;

		free_filter_pattern(&patt);
	}

	if (stats.failed + stats.skipped + stats.nomatch == 0) {
		pr_dbg("patched all (%d) functions in '%s'\n",
		       stats.total, basename(symtabs->filename));
	}

	strv_free(&funcs);
	return 0;
}

static void finish_dynamic_update(struct mcount_disasm_engine *disasm)
{
	struct mcount_dynamic_info *mdi, *tmp;

	mdi = mdinfo;
	while (mdi) {
		tmp = mdi->next;

		mcount_cleanup_trampoline(mdi);
		free(mdi->mod_name);
		free(mdi);

		mdi = tmp;
	}

	mcount_disasm_finish(disasm);
	mcount_freeze_code();
}

/* do not use floating-point in libmcount */
static int calc_percent(int n, int total, int *rem)
{
	int quot = 100 * n / total;

	*rem = (100 * n - quot * total) * 100 / total;
	return quot;
}

int mcount_dynamic_update(struct symtabs *symtabs, char *patch_funcs,
			  enum uftrace_pattern_type ptype,
			  struct mcount_disasm_engine *disasm)
{
	int ret = 0;

	prepare_dynamic_update(disasm, symtabs);
	ret = do_dynamic_update(symtabs, patch_funcs, ptype, disasm);

	if (stats.total && stats.failed) {
		int success = stats.total - stats.failed - stats.skipped;
		int r, q;

		pr_dbg("dynamic patch stats for '%s'\n",
		       basename(symtabs->filename));
		pr_dbg("   total: %8d\n", stats.total);
		q = calc_percent(success, stats.total, &r);
		pr_dbg(" patched: %8d (%2d.%02d%%)\n", success, q, r);
		q = calc_percent(stats.failed, stats.total, &r);
		pr_dbg("  failed: %8d (%2d.%02d%%)\n", stats.failed, q, r);
		q = calc_percent(stats.skipped, stats.total, &r);
		pr_dbg(" skipped: %8d (%2d.%02d%%)\n", stats.skipped, q, r);
		pr_dbg("no match: %8d\n", stats.nomatch);
	}

	finish_dynamic_update(disasm);
	return ret;
}
