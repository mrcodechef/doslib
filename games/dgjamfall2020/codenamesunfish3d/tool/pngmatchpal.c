
#include <stdio.h>
#include <string.h>

#include <png.h>    /* libpng */

static char*        pal_png = NULL;
static char*        in_png = NULL;
static char*        out_png = NULL;

static png_color    pal_png_pal[256] = {0};
static int          pal_png_pal_count = 0;

static unsigned char    pal_remap[256] = {0};

static png_color        gen_png_pal[256] = {0};
static int              gen_png_pal_count = 0;

static unsigned char*   gen_png_image = NULL;
static png_bytep        gen_png_image_rows = NULL;
png_uint_32             gen_png_width = 0,gen_png_height = 0;
int                     gen_png_bit_depth = 0;
int                     gen_png_color_type = 0;
int                     gen_png_interlace_method = 0;
int                     gen_png_compression_method = 0;
int                     gen_png_filter_method = 0;
 
static void help(void) {
    fprintf(stderr,"pngmatchpal -i <input PNG> -o <output PNG> -p <palette PNG>\n");
    fprintf(stderr,"Convert a paletted PNG to another paletted PNG,\n");
    fprintf(stderr,"rearranging the palette to match palette PNG.\n");
}

static int parse_argv(int argc,char **argv) {
    int i = 1;
    char *a;

    while (i < argc) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help();
                return 1;
            }
            else if (!strcmp(a,"i")) {
                if ((in_png = argv[i++]) == NULL)
                    return 1;
            }
            else if (!strcmp(a,"o")) {
                if ((out_png = argv[i++]) == NULL)
                    return 1;
            }
            else if (!strcmp(a,"p")) {
                if ((pal_png = argv[i++]) == NULL)
                    return 1;
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unexpected arg %s\n",a);
            return 1;
        }
    }

    if (in_png == NULL) {
        fprintf(stderr,"Input -i png required\n");
        return 1;
    }

    if (out_png == NULL) {
        fprintf(stderr,"Output -o png required\n");
        return 1;
    }

    if (pal_png == NULL) {
        fprintf(stderr,"Palette -p png required\n");
        return 1;
    }

    return 0;
}

static int load_palette_png(void) {
    png_structp png_context = NULL;
    png_infop png_context_info = NULL;
    png_infop png_context_end = NULL;
    png_uint_32 png_width = 0,png_height = 0;
    int png_bit_depth = 0;
    int png_color_type = 0;
    int png_interlace_method = 0;
    int png_compression_method = 0;
    int png_filter_method = 0;
    FILE *fp = NULL;
    int ret = 1;

    if (pal_png == NULL)
        return 1;

    fp = fopen(pal_png,"rb");
    if (fp == NULL)
        return 1;

    png_context = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL/*error*/,NULL/*error fn*/,NULL/*warn fn*/);
    if (png_context == NULL) goto fail;

    png_context_info = png_create_info_struct(png_context);
    if (png_context_info == NULL) goto fail;

    png_init_io(png_context, fp);
    png_read_info(png_context, png_context_info);

    if (!png_get_IHDR(png_context, png_context_info, &png_width, &png_height, &png_bit_depth, &png_color_type, &png_interlace_method, &png_compression_method, &png_filter_method))
        goto fail;

    if (!(png_color_type & PNG_COLOR_MASK_PALETTE)) {
        fprintf(stderr,"Palette PNG not paletted\n");
        goto fail;
    }

    {
        png_color* pal = NULL;
        int pal_count = 0;

        /* FIXME: libpng makes no reference to freeing this. Do you? */
        if (png_get_PLTE(png_context, png_context_info, &pal, &pal_count) == 0) {
            fprintf(stderr,"Unable to get Palette PNG palette\n");
            goto fail;
        }

        /* I think libpng only points at it's in memory buffers. Copy it. */
        pal_png_pal_count = pal_count;
        if (pal_count != 0 && pal_count <= 256)
            memcpy(pal_png_pal,pal,sizeof(png_color) * pal_count);
    }

    /* success */
    ret = 0;
fail:
    if (png_context != NULL)
        png_destroy_read_struct(&png_context,&png_context_info,&png_context_end);

    if (ret)
        fprintf(stderr,"Failed to load palette PNG\n");

    fclose(fp);
    return ret;
}

int main(int argc,char **argv) {
    if (parse_argv(argc,argv))
        return 1;

    if (load_palette_png())
        return 1;

    return 0;
}
