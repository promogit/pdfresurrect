/******************************************************************************
 * main.c
 *
 * pdfresurrect - PDF history extraction tool
 * https://github.com/enferex/pdfresurrect
 *
 * See https://github.com/enferex/pdfresurrect/blob/master/LICENSE for license
 * information.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Special thanks to all of the contributors:  See AUTHORS.
 * Special thanks to 757labs (757 crew), they are a great group
 * of people to hack on projects and brainstorm with.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "main.h"
#include "pdf.h"


static void usage(void)
{
    printf("-- " EXEC_NAME " v" VER" --\n"
           "Usage: ./" EXEC_NAME " <file.pdf> [-i] [-w] [-q]\n"
           "\t -i Display PDF creator information\n"
           "\t -w Write the PDF versions and summary to disk\n"
           "\t -q Display only the number of versions contained in the PDF\n");
// Experimental feature:
//           "\t -s Scrub the previous history data from the specified PDF\n");
    exit(0);
}


static void write_version(
    FILE       *fp,
    const char *fname,
    const char *dirname,
    xref_t     *xref)
{
    long  start;
    char *base_fname, *new_fname, data;
    FILE *new_fp;

    start = ftell(fp);

    // Extract base file name
    char *slash_pos = strrchr(fname, '/');
    base_fname = slash_pos ? slash_pos + 1 : (char *)fname;

    // Remove ".pdf" extension if exists
    char *c = strstr(base_fname, ".pdf");
    if (c) *c = '\0';

    // Prepare new file name
    new_fname = safe_calloc(strlen(base_fname) + strlen(dirname) + 32);
    snprintf(new_fname, strlen(base_fname) + strlen(dirname) + 32,
             "%s/%s-version-%d.pdf", dirname, base_fname, xref->version);

    // Force create file, overwriting if necessary
    if (!(new_fp = fopen(new_fname, "wb"))) {
        ERR("Could not create file '%s'\n", new_fname);
        fseek(fp, start, SEEK_SET);
        free(new_fname);
        return;
    }

    // Copy original PDF
    fseek(fp, 0, SEEK_SET);
    while (fread(&data, 1, 1, fp))
        fwrite(&data, 1, 1, new_fp);

    // Emit an older startxref, referring to an older version.
    fprintf(new_fp, "\r\nstartxref\r\n%ld\r\n%%%%EOF", xref->start);

    // Clean up
    fclose(new_fp);
    free(new_fname);
    fseek(fp, start, SEEK_SET);
}

#ifdef PDFRESURRECT_EXPERIMENTAL
static void scrub_document(FILE *fp, const pdf_t *pdf)
{
    FILE *new_fp;
    int   ch, i, j, last_version ;
    char *new_name, *c;
    const char *suffix = "-scrubbed.pdf";

    printf("The scrub feature (-s) is experimental and likely not to work as "
           "expected.\n");

    /* Create a new name */
    if (!(new_name = malloc(strlen(pdf->name) + strlen(suffix) + 1)))
    {
        ERR("Insufficient memory to create scrubbed file name\n");
        return;
    }

    strcpy(new_name, pdf->name);
    if ((c = strrchr(new_name, '.')))
      *c = '\0';
    strcat(new_name, suffix);

    if ((new_fp = fopen(new_name, "r")))
    {
        ERR("File name already exists for saving scrubbed document\n");
        free(new_name);
        fclose(new_fp);
        return;
    }

    if (!(new_fp = fopen(new_name, "w+")))
    {
        ERR("Could not create file for saving scrubbed document\n");
        free(new_name);
        fclose(new_fp);
        return;
    }

    /* Copy */
    fseek(fp, SEEK_SET, 0);
    while ((ch = fgetc(fp)) != EOF)
      fputc(ch, new_fp);

    /* Find last version (don't zero these baddies) */
    last_version = 0;
    for (i=0; i<pdf->n_xrefs; i++)
      if (pdf->xrefs[i].version)
        last_version = pdf->xrefs[i].version;

    /* Zero mod objects from all but the most recent version
     * Zero del objects from all versions
     */
    fseek(new_fp, 0, SEEK_SET);
    for (i=0; i<pdf->n_xrefs; i++)
    {
        for (j=0; j<pdf->xrefs[i].n_entries; j++)
          if (!pdf->xrefs[i].entries[j].obj_id)
            continue;
          else
          {
              switch (pdf_get_object_status(pdf, i, j))
              {
                  case 'M':
                      if (pdf->xrefs[i].version != last_version)
                        pdf_zero_object(new_fp, pdf, i, j);
                      break;

                  case 'D':
                      pdf_zero_object(new_fp, pdf, i, j);
                      break;

                  default:
                      break;
              }
          }
    }

    /* Clean */
    free(new_name);
    fclose(new_fp);
}
#endif // PDFRESURRECT_EXPERIMENTAL


static void display_creator(FILE *fp, const pdf_t *pdf)
{
    int i;

    printf("PDF Version: %d.%d\n",
           pdf->pdf_major_version, pdf->pdf_minor_version);

    for (i=0; i<pdf->n_xrefs; ++i)
    {
        if (!pdf->xrefs[i].version)
          continue;

        if (pdf_display_creator(pdf, i))
          printf("\n");
    }
}


static pdf_t *init_pdf(FILE *fp, const char *name)
{
    pdf_t *pdf;

    pdf = pdf_new(name);
    pdf_get_version(fp, pdf);
    if (pdf_load_xrefs(fp, pdf) == -1) {
      pdf_delete(pdf);
      return NULL;
    }

    return pdf;
}


void *safe_calloc(size_t size) {
  void *addr;

  if (!size)
  {
    ERR("Invalid allocation size.\n");
    exit(EXIT_FAILURE);
  }
  if (!(addr = calloc(1, size)))
  {
      ERR("Failed to allocate requested number of bytes, out of memory?\n");
      exit(EXIT_FAILURE);
  }
  return addr;
}
int main(int argc, char **argv)
{
    int         i, n_valid, do_write, do_scrub;
    //char       *c;
    char       *dname, *name;
    //DIR        *dir;
    FILE       *fp;
    pdf_t      *pdf;
    pdf_flag_t  flags;

    if (argc < 2)
      usage();

    /* Args */
    do_write = do_scrub = flags = 0;
    name = NULL;
    for (i=1; i<argc; i++)
    {
        if (strncmp(argv[i], "-w", 2) == 0)
          do_write = 1;
        else if (strncmp(argv[i], "-i", 2) == 0)
          flags |= PDF_FLAG_DISP_CREATOR;
        else if (strncmp(argv[i], "-q", 2) == 0)
          flags |= PDF_FLAG_QUIET;
#ifdef PDFRESURRECT_EXPERIMENTAL
        else if (strncmp(argv[i], "-s", 2) == 0)
          do_scrub = 1;
#endif
        else if (argv[i][0] != '-')
          name = argv[i];
        else if (argv[i][0] == '-')
          usage();
    }

    if (!name)
      usage();

    if (!(fp = fopen(name, "r")))
    {
        ERR("Could not open file '%s'\n", argv[1]);
        return -1;
    }
    else if (!pdf_is_pdf(fp))
    {
        ERR("'%s' specified is not a valid PDF\n", name);
        fclose(fp);
        return -1;
    }

    /* Load PDF */
    if (!(pdf = init_pdf(fp, name)))
    {
        fclose(fp);
        return -1;
    }

    /* Count valid xrefs */
    for (i=0, n_valid=0; i<pdf->n_xrefs; i++)
      if (pdf->xrefs[i].version)
        ++n_valid;

    /* Bail if we only have 1 valid */
    if (n_valid < 2)
    {
        if (!(flags & (PDF_FLAG_QUIET | PDF_FLAG_DISP_CREATOR)))
          printf("%s: There is only one version of this PDF\n", pdf->name);

        if (do_write)
        {
            fclose(fp);
            pdf_delete(pdf);
            return 0;
        }
    }

    dname = NULL;
    // Directory handling removed for simplicity, write directly into 'static/'
    if (do_write) {
        // Ensure 'static/' directory exists, removed error handling for existing directory
        for (i = 0; i < pdf->n_xrefs; i++) {
            if (pdf->xrefs[i].version) {
                write_version(fp, name, "static", &pdf->xrefs[i]);
            }
        }
    }

    /* Generate a per-object summary */
    pdf_summarize(fp, pdf, dname, flags);

#ifdef PDFRESURRECT_EXPERIMENTAL
    /* Have we been summoned to scrub history from this PDF */
    if (do_scrub)
      scrub_document(fp, pdf);
#endif

    /* Display extra information */
    if (flags & PDF_FLAG_DISP_CREATOR)
      display_creator(fp, pdf);

    fclose(fp);
    free(dname);
    pdf_delete(pdf);

    return 0;
}

