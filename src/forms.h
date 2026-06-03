/* Nordstjernen — HTML form validation, serialization, and submission helpers. */

#ifndef ND_FORMS_H
#define ND_FORMS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

gboolean nd_form_is_submit_trigger(const nd_node *n);
gboolean nd_form_is_reset_trigger(const nd_node *n);

void nd_clear_radio_group_for(const nd_node *doc, const nd_node *keep);

gboolean nd_form_has_file_upload(const nd_node *form, const nd_node *n,
                                 const nd_node *doc);

void nd_form_collect_multipart(const nd_node *form, const nd_node *n,
                               const nd_node *doc, GString *body,
                               const char *boundary, const nd_node *submitter);
void nd_form_collect_inputs(const nd_node *form, const nd_node *n,
                            const nd_node *doc, GString *query,
                            gboolean *first, const nd_node *submitter);

const nd_node *nd_form_first_invalid(const nd_node *form, const nd_node *n,
                                     const nd_node *doc);

G_END_DECLS

#endif
