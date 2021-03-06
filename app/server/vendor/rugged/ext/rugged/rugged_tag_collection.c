/*
 * The MIT License
 *
 * Copyright (c) 2014 GitHub, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "rugged.h"

extern VALUE rb_mRugged;
extern VALUE rb_cRuggedRepo;
extern VALUE rb_cRuggedTag;

VALUE rb_cRuggedTagCollection;

/*
 *  call-seq:
 *    TagCollection.new(repo) -> refs
 */
static VALUE rb_git_tag_collection_initialize(VALUE self, VALUE repo)
{
	rugged_set_owner(self, repo);
	return self;
}

/*
 *  call-seq:
 *    tags[name] -> tag
 *
 *  Lookup a tag in +repo+, with the given +name+.
 *
 *  +name+ can be a short or canonical tag name
 *  (e.g. +v0.1.0+ or +refs/tags/v0.1.0+).
 *
 *  Returns the looked up tag, or +nil+ if the tag doesn't exist.
 */
static VALUE rb_git_tag_collection_aref(VALUE self, VALUE rb_name)
{
	git_reference *tag;
	git_repository *repo;
	int error;
	VALUE rb_repo = rugged_owner(self);

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	Check_Type(rb_name, T_STRING);

	error = git_reference_lookup(&tag, repo, StringValueCStr(rb_name));
	if (error == GIT_ENOTFOUND || error == GIT_EINVALIDSPEC) {
		char *canonical_ref = xmalloc((RSTRING_LEN(rb_name) + strlen("refs/tags/") + 1) * sizeof(char));
		strcpy(canonical_ref, "refs/tags/");
		strcat(canonical_ref, StringValueCStr(rb_name));

		error = git_reference_lookup(&tag, repo, canonical_ref);
		xfree(canonical_ref);
		if (error == GIT_ENOTFOUND)
			return Qnil;
	}

	rugged_exception_check(error);

	return rugged_ref_new(rb_cRuggedTag, rb_repo, tag);
}

/*
 *  call-seq:
 *    tags.delete(name) -> nil
 *
 *  Delete the tag reference identified by +name+.
 */
static VALUE rb_git_tag_collection_delete(VALUE self, VALUE rb_name)
{
	VALUE rb_repo = rugged_owner(self);
	git_repository *repo;
	int error;

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	Check_Type(rb_name, T_STRING);

	error = git_tag_delete(repo, StringValueCStr(rb_name));
	rugged_exception_check(error);
	return Qnil;
}

/*
 *  call-seq:
 *    tags.create(name, target[, force = false][, annotation = nil]) -> oid
 *
 *  Create a new tag with the specified +name+ on +target+ in +repo+.
 *
 *  If +annotation+ is not +nil+, it will cause the creation of an annotated tag object.
 *  +annotation+ has to contain the following key value pairs:
 *
 *  :tagger ::
 *    An optional Hash containing a git signature. Defaults to the signature
 *    from the configuration if only `:message` is given. Will cause the
 *    creation of an annotated tag object if present.
 *
 *  :message ::
 *    An optional string containing the message for the new tag.
 *
 *  Returns the OID of the newly created tag.
 */
static VALUE rb_git_tag_collection_create(int argc, VALUE *argv, VALUE self)
{
	git_oid tag_oid;
	git_repository *repo = NULL;
	git_object *target = NULL;
	int error, force = 0;

	VALUE rb_repo = rugged_owner(self), rb_name, rb_target, rb_force, rb_annotation;

	rb_scan_args(argc, argv, "21:", &rb_name, &rb_target, &rb_force, &rb_annotation);

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	Check_Type(rb_name, T_STRING);

	if (!NIL_P(rb_force))
		force = rugged_parse_bool(rb_force);

	target = rugged_object_get(repo, rb_target, GIT_OBJ_ANY);

	if (NIL_P(rb_annotation)) {
		error = git_tag_create_lightweight(
			&tag_oid,
			repo,
			StringValueCStr(rb_name),
			target,
			force
		);
	} else {
		git_signature *tagger = rugged_signature_get(
			rb_hash_aref(rb_annotation, CSTR2SYM("tagger")), repo
		);
		VALUE rb_message = rb_hash_aref(rb_annotation, CSTR2SYM("message"));

		Check_Type(rb_message, T_STRING);

		error = git_tag_create(
			&tag_oid,
			repo,
			StringValueCStr(rb_name),
			target,
			tagger,
			StringValueCStr(rb_message),
			force
		);

		git_signature_free(tagger);
	}

	git_object_free(target);
	rugged_exception_check(error);

	return rb_git_tag_collection_aref(self, rb_name);
}

static VALUE each_tag(int argc, VALUE *argv, VALUE self, int tag_names_only)
{
	git_repository *repo;
	git_strarray tags;
	size_t i;
	int error, exception = 0;
	VALUE rb_repo = rugged_owner(self), rb_pattern;
	const char *pattern = NULL;

	rb_scan_args(argc, argv, "01", &rb_pattern);

	if (!rb_block_given_p()) {
		VALUE symbol = tag_names_only ? CSTR2SYM("each_name") : CSTR2SYM("each");
		return rb_funcall(self, rb_intern("to_enum"), 2, symbol, rb_pattern);
	}

	if (!NIL_P(rb_pattern)) {
		Check_Type(rb_pattern, T_STRING);
		pattern = StringValueCStr(rb_pattern);
	}

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	error = git_tag_list_match(&tags, pattern ? pattern : "", repo);
	rugged_exception_check(error);

	if (tag_names_only) {
		for (i = 0; !exception && i < tags.count; ++i)
			rb_protect(rb_yield, rb_str_new_utf8(tags.strings[i]), &exception);
	} else {
		for (i = 0; !exception && i < tags.count; ++i) {
			rb_protect(rb_yield, rb_git_tag_collection_aref(self,
				rb_str_new_utf8(tags.strings[i])), &exception);
		}
	}

	git_strarray_free(&tags);

	if (exception)
		rb_jump_tag(exception);

	return Qnil;
}

/*
 *  call-seq:
 *    tags.each_name([pattern]) { |name| block } -> nil
 *    tags.each_name([pattern])                  -> enumerator
 *
 *  Iterate through all the tag names in +repo+. Iteration
 *  can be optionally filtered to the ones matching the given
 *  +pattern+, a standard Unix filename glob.
 *
 *  If +pattern+ is empty or not given, all tag names will be returned.
 *
 *  The given block will be called once with the name for each tag.
 *
 *  If no block is given, an enumerator will be returned.
 */
static VALUE rb_git_tag_collection_each_name(int argc, VALUE *argv, VALUE self)
{
	return each_tag(argc, argv, self, 1);
}

/*
 *  call-seq:
 *    tags.each([pattern]) { |name| block } -> nil
 *    tags.each([pattern])                  -> enumerator
 *
 *  Iterate through all the tags in +repo+. Iteration
 *  can be optionally filtered to the ones matching the given
 *  +pattern+, a standard Unix filename glob.
 *
 *  If +pattern+ is empty or not given, all tag names will be returned.
 *
 *  The given block will be called once with the name for each tag.
 *
 *  If no block is given, an enumerator will be returned.
 */
static VALUE rb_git_tag_collection_each(int argc, VALUE *argv, VALUE self)
{
	return each_tag(argc, argv, self, 0);
}

void Init_rugged_tag_collection(void)
{
	rb_cRuggedTagCollection = rb_define_class_under(rb_mRugged, "TagCollection", rb_cObject);
	rb_include_module(rb_cRuggedTagCollection, rb_mEnumerable);

	rb_define_method(rb_cRuggedTagCollection, "initialize", rb_git_tag_collection_initialize, 1);

	rb_define_method(rb_cRuggedTagCollection, "create",     rb_git_tag_collection_create, -1);
	rb_define_method(rb_cRuggedTagCollection, "[]",         rb_git_tag_collection_aref, 1);

	rb_define_method(rb_cRuggedTagCollection, "each",       rb_git_tag_collection_each, -1);
	rb_define_method(rb_cRuggedTagCollection, "each_name",  rb_git_tag_collection_each_name, -1);

	rb_define_method(rb_cRuggedTagCollection, "delete",     rb_git_tag_collection_delete, 1);
}
