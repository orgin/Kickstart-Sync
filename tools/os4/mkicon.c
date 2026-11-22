/* mkicon - lets icon.library save an icon from a raw 64x64 ARGB picture.
 * Used once to create the release icons in amiga/package/icons (see
 * tools/make_icons.py); not part of the release. Public domain.
 *
 *   mkicon NAME ARGB=file [SELECTED=file] TYPE=PROJECT|DRAWER|TOOL
 *          [TOOL=default tool] [STACK=n] [WINDOW=x,y,w,h] [TT=tooltype ...]
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IconIFace *IIcon;

#define W 64
#define H 64

static unsigned char *load(const char *path)
{
	unsigned char *buf = malloc(W * H * 4);
	FILE *f = fopen(path, "rb");

	if (buf == NULL || f == NULL || fread(buf, 1, W * H * 4, f) != W * H * 4) {
		printf("cannot read %s\n", path);
		exit(10);
	}
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	const char *name = NULL, *argb = NULL, *sel = NULL, *tool = NULL, *type = "PROJECT";
	char *tts[16];
	int ntt = 0, stack = 0, wx = -1, wy = 0, ww = 0, wh = 0, i, err = 0;
	struct Library *IconBase;
	struct DiskObject *dobj;
	unsigned char *img1, *img2 = NULL;
	STRPTR old_tool;
	STRPTR *old_tts;
	int t;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "ARGB=", 5) == 0) argb = argv[i] + 5;
		else if (strncmp(argv[i], "SELECTED=", 9) == 0) sel = argv[i] + 9;
		else if (strncmp(argv[i], "TYPE=", 5) == 0) type = argv[i] + 5;
		else if (strncmp(argv[i], "TOOL=", 5) == 0) tool = argv[i] + 5;
		else if (strncmp(argv[i], "STACK=", 6) == 0) stack = atoi(argv[i] + 6);
		else if (strncmp(argv[i], "WINDOW=", 7) == 0)
			sscanf(argv[i] + 7, "%d,%d,%d,%d", &wx, &wy, &ww, &wh);
		else if (strncmp(argv[i], "TT=", 3) == 0 && ntt < 15) tts[ntt++] = argv[i] + 3;
		else name = argv[i];
	}
	tts[ntt] = NULL;
	if (name == NULL || argb == NULL) {
		printf("usage: mkicon NAME ARGB=file [SELECTED=file] TYPE=PROJECT|DRAWER|TOOL "
		       "[TOOL=x] [STACK=n] [WINDOW=x,y,w,h] [TT=x ...]\n");
		return 5;
	}
	t = strcmp(type, "DRAWER") == 0 ? WBDRAWER : strcmp(type, "TOOL") == 0 ? WBTOOL : WBPROJECT;

	IconBase = IExec->OpenLibrary("icon.library", 51);
	if (IconBase == NULL)
		return 20;
	IIcon = (struct IconIFace *)IExec->GetInterface(IconBase, "main", 1, NULL);

	img1 = load(argb);
	if (sel != NULL)
		img2 = load(sel);

	/* Start from the default icon of this type, which is already a native
	 * true colour icon, and replace its picture: an icon made with
	 * NewDiskObject() cannot be saved with a true colour picture. */
	dobj = IIcon->GetDefDiskObject(t);
	if (dobj == NULL) {
		printf("GetDefDiskObject failed\n");
		return 10;
	}
	if (!IIcon->IconControl(dobj,
	                        ICONCTRLA_SetImageDataFormat, IDFMT_DIRECTMAPPED,
	                        ICONCTRLA_SetWidth, W,
	                        ICONCTRLA_SetHeight, H,
	                        ICONCTRLA_SetImageData1, img1,
	                        ICONCTRLA_SetImageData2, img2,
	                        TAG_END))
		printf("IconControl failed\n");
	dobj->do_Gadget.Width = W;
	dobj->do_Gadget.Height = H;

	old_tool = dobj->do_DefaultTool;
	old_tts = dobj->do_ToolTypes;
	dobj->do_DefaultTool = (STRPTR)tool;
	dobj->do_ToolTypes = ntt > 0 ? (STRPTR *)tts : NULL;
	if (stack > 0)
		dobj->do_StackSize = stack;
	dobj->do_CurrentX = NO_ICON_POSITION;
	dobj->do_CurrentY = NO_ICON_POSITION;
	if (t == WBDRAWER && dobj->do_DrawerData != NULL && wx >= 0) {
		dobj->do_DrawerData->dd_NewWindow.LeftEdge = wx;
		dobj->do_DrawerData->dd_NewWindow.TopEdge = wy;
		dobj->do_DrawerData->dd_NewWindow.Width = ww;
		dobj->do_DrawerData->dd_NewWindow.Height = wh;
	}

	/* drop the planar fallback picture of the default icon: only our
	 * picture goes into the file */
	if (!IIcon->PutIconTags(name, dobj, ICONPUTA_DropPlanarIconImage, TRUE,
	                        ICONA_ErrorCode, &err, TAG_END))
		printf("PutIcon %s failed, error %d\n", name, err);
	else
		printf("%s.info written\n", name);

	dobj->do_DefaultTool = old_tool;
	dobj->do_ToolTypes = old_tts;
	IIcon->FreeDiskObject(dobj);
	IExec->DropInterface((struct Interface *)IIcon);
	IExec->CloseLibrary(IconBase);
	return err ? 10 : 0;
}
