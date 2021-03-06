#include "quakedef.h"
#ifdef SWQUAKE
#include "sw.h"
#include "gl_draw.h"
#include "shader.h"
#include "renderque.h"
#include "glquake.h"

#if __STDC_VERSION__ >= 199901L
	//no need to do anything
#elif defined(_MSC_VER)
	#define restrict __restrict
#else
	#define restrict
#endif

/*
Our software rendering basically works like this:

main thread builds command:
	command contains vertex data in the command block
	main thread runs the vertex programs (much like q3) and performs matrix transforms (much like d3d)

worker threads read each command sequentially:
	clip to viewport

division of labour between worker threads works by interlacing.
each thread gets a different set of scanlines to render.
we can also trivially implement interlacing with this method

*/

cvar_t sw_interlace = CVAR("sw_interlace", "0");
cvar_t sw_vthread = CVAR("sw_vthread", "0");
cvar_t sw_fthreads = CVAR("sw_fthreads", "0");

struct workqueue_s commandqueue;
struct workqueue_s spanqueue;

static void WT_Triangle(swthread_t *th, swimage_t *img, swvert_t *v1, swvert_t *v2, swvert_t *v3)
{
	unsigned int tpix;
#define SPAN_ST
#define SPAN_Z
#define PLOT_PIXEL(o) \
	{	\
		if (*zb >= z)	\
		{	\
			*zb = z;	\
		tpix = img->data[	\
					((unsigned)(s*img->width)%img->width)	\
					+ (((unsigned)(t*img->height)%img->height) * img->width)	\
				];	\
		if (tpix&0xff000000) \
			o = tpix; \
		}	\
	}

#ifdef MSVCWORKSPROPERLY
#include "sw_spans.h"
#else
/*
this file is expected to be #included as the body of a real function
to define create a new pixel shader, define PLOT_PIXEL(outval) at the top of your function and you're good to go

//modifiers:
SPAN_ST - interpolates S+T across the span. access with 'sc' and 'tc'
		affine... no perspective correction.


*/

{
	swvert_t *vt;
	int y;
	int secondhalf;
	int xl,xld, xr,xrd;
#ifdef SPAN_ST
	float sl,sld, sd;
	float tl,tld, td;
#endif
#ifdef SPAN_Z
	unsigned int zl,zld, zd;
#endif
	unsigned int *restrict outbuf;
	unsigned int *restrict ti;
	int i;
	const swvert_t *vlt,*vlb,*vrt,*vrb;
	int spanlen;
	int numspans;
	unsigned int *vplout;
	int dx, dy;
	int recalcside;
	int interlace;

	float fdx1,fdy1,fdx2,fdy2,fz,d1,d2;

	if (!img)
		return;

	/*we basically render a diamond
	that is, the single triangle is split into two triangles, outwards towards the midpoint and inwards to the final position.
	*/

	/*reorder the verticies for height*/
	if (v1->vcoord[1] > v2->vcoord[1])
	{
		vt = v1;
		v1 = v2;
		v2 = vt;
	}
	if (v1->vcoord[1] > v3->vcoord[1])
	{
		vt = v1;
		v1 = v3;
		v3 = vt;
	}
	if (v2->vcoord[1] > v3->vcoord[1])
	{
		vt = v3;
		v3 = v2;
		v2 = vt;
	}

	{
		const swvert_t *v[3];

		v[0] = v1;
		v[1] = v2;
		v[2] = v3;

		//reject triangles with any point offscreen, for now
		for (i = 0; i < 3; i++)
		{
			if (v[i]->vcoord[0] < 0 || v[i]->vcoord[0] >= th->vpwidth)
				return;
			if (v[i]->vcoord[1] < 0 || v[i]->vcoord[1] >= th->vpheight)
				return;
			if (v[i]->vcoord[2] < 0)
				return;
		}

		for (i = 0; i < 2; i++)
		{
			if (v[i]->vcoord[1] > v[i+1]->vcoord[1])
				return;
		}
	}

	fdx1 = v2->vcoord[0] - v1->vcoord[0];
	fdy1 = v2->vcoord[1] - v1->vcoord[1];

	fdx2 = v3->vcoord[0] - v1->vcoord[0];
	fdy2 = v3->vcoord[1] - v1->vcoord[1];

	fz = fdx1*fdy2 - fdx2*fdy1;

	if (fz == 0)
	{
		//weird angle...
		return;
	}

	fz = 1.0 / fz;
	fdx1 *= fz;
	fdy1 *= fz;
	fdx2 *= fz;
	fdy2 *= fz;

#ifdef SPAN_ST	//affine
	d1 = v2->tccoord[0] - v1->tccoord[0];
	d2 = v3->tccoord[0] - v1->tccoord[0];
	sld = fdx1*d2 - fdx2*d1;
	sd = fdy2*d1 - fdy1*d2;

	d1 = v2->tccoord[1] - v1->tccoord[1];
	d2 = v3->tccoord[1] - v1->tccoord[1];
	tld = fdx1*d2 - fdx2*d1;
	td = fdy2*d1 - fdy1*d2;
#endif
#ifdef SPAN_Z
	d1 = (v2->vcoord[2] - v1->vcoord[2])*UINT_MAX;
	d2 = (v3->vcoord[2] - v1->vcoord[2])*UINT_MAX;
	zld = fdx1*d2 - fdx2*d1;
	zd = fdy2*d1 - fdy1*d2;
#endif

	ti = img->data;

	y = v1->vcoord[1];

	for (secondhalf = 0; secondhalf <= 1; secondhalf++)
	{
		if (secondhalf)
		{
			if (numspans < 0)
			{
				interlace = -numspans;
				y+=interlace;
				numspans-=interlace;

				xl += xld*interlace;
				xr += xrd*interlace;
				vplout += th->vpcstride*interlace;

#ifdef SPAN_ST
				sl += sld*interlace;
				tl += tld*interlace;
#endif
#ifdef SPAN_Z
				zl += zld*interlace;
#endif
			}

			/*v2->v3*/
			if (fz <= 0)
			{
				vlt = v2;
				//vrt == v1;
				vlb = v3;
				//vrb == v3;

				recalcside = 1;

#ifdef SPAN_ST
				sld -= sd*xld/(float)(1<<16);
				tld -= td*xld/(float)(1<<16);
#endif
#ifdef SPAN_Z
				zld -= zd*xld/(float)(1<<16);
#endif
			}
			else
			{
				//vlt == v1;
				vrt = v2;
				///vlb == v3;
				vrb = v3;

				recalcside = 2;
			}

			//flip the triangle to keep it facing the screen (we swapped the verts almost randomly)
			numspans = v3->vcoord[1] - y;
		}
		else
		{
			vlt = v1;
			vrt = v1;
			/*v1->v2*/
			if (fz < 0)
			{
				vlb = v2;
				vrb = v3;
			}
			else
			{
				vlb = v3;
				vrb = v2;
			}
			recalcside = 3;

			//flip the triangle to keep it facing the screen (we swapped the verts almost randomly)
			numspans = v2->vcoord[1] - y;
		}

		if (recalcside & 1)
		{
			dx = (vlb->vcoord[0] - vlt->vcoord[0]);
			dy = (vlb->vcoord[1] - vlt->vcoord[1]);
			if (dy > 0)
				xld = (dx<<16) / dy;
			else
				xld = 0;
			xl = (int)vlt->vcoord[0]<<16;

#ifdef SPAN_ST
			sl = vlt->tccoord[0];
			sld = sld + sd*xld/(float)(1<<16);
			tl = vlt->tccoord[1];
			tld = tld + td*xld/(float)(1<<16);
#endif
#ifdef SPAN_Z
			zl = vlt->vcoord[2]*UINT_MAX;
			zld = zld + zd*xld/(float)(1<<16);
#endif
		}

		if (recalcside & 2)
		{
			dx = (vrb->vcoord[0] - vrt->vcoord[0]);
			dy = (vrb->vcoord[1] - vrt->vcoord[1]);
			if (dy)
				xrd = (dx<<16) / dy;
			else
				xrd = 0;
			xr = (int)vrt->vcoord[0]<<16;
		}



		if (y + numspans >= th->vpheight)
			numspans = th->vpheight - y - 1;

		if (numspans <= 0)
			continue;


		vplout = th->vpcbuf + y * th->vpcstride;	//this is a pointer to the left of the viewport buffer.

		interlace = ((y + th->interlaceline) % th->interlacemod);
		if (interlace)
		{
			if (interlace > numspans)
			{
				interlace = numspans;
				y+=interlace;
			}
			else
			{
				y+=interlace;
				numspans-=interlace;
			}
			xl += xld*interlace;
			xr += xrd*interlace;
			vplout += th->vpcstride*interlace;

#ifdef SPAN_ST
			sl += sld*interlace;
			tl += tld*interlace;
#endif
#ifdef SPAN_Z
			zl += zld*interlace;
#endif
		}

		for (; numspans > 0; 
			numspans -= th->interlacemod
			,xl += xld*th->interlacemod
			,xr += xrd*th->interlacemod
			,vplout += th->vpcstride*th->interlacemod
			,y += th->interlacemod

#ifdef SPAN_ST
			,sl += sld*th->interlacemod
			,tl += tld*th->interlacemod
#endif
#ifdef SPAN_Z
			,zl += zld*th->interlacemod
#endif
			)
		{
#ifdef SPAN_ST
			float s = sl;
			float t = tl;
#endif
#ifdef SPAN_Z
			unsigned int z = zl;
			unsigned int *restrict zb = th->vpdbuf + y * th->vpwidth + (xl>>16);
#endif

			spanlen = (xr - xl)>>16;
			outbuf = vplout + (xl>>16);

			while(spanlen-->=0)
			{
				PLOT_PIXEL(*outbuf);
				outbuf++;

#ifdef SPAN_ST
				s += sd;
				t += td;
#endif
#ifdef SPAN_Z
				z += zd;
				zb++;
#endif
			}
		}
	}
}

#undef SPAN_ST
#undef PLOT_PIXEL
#endif
	
}

static void WT_Clip_Top(swvert_t *out, swvert_t *in, swvert_t *result)
{
	float frac;
	frac =	(0 - in->vcoord[1]) /
			(out->vcoord[1] - in->vcoord[1]);
	VectorInterpolate(in->vcoord, frac, out->vcoord, result->vcoord);
	result->vcoord[1] = 0;
	Vector2Interpolate(in->tccoord, frac, out->tccoord, result->tccoord);
}
static void WT_Clip_Bottom(swvert_t *out, swvert_t *in, swvert_t *result)
{
	float frac;
	frac =	(vid.pixelheight-1 - in->vcoord[1]) /
			(out->vcoord[1] - in->vcoord[1]);
	VectorInterpolate(in->vcoord, frac, out->vcoord, result->vcoord);
	result->vcoord[1] = vid.pixelheight-1;
	Vector2Interpolate(in->tccoord, frac, out->tccoord, result->tccoord);
}
static void WT_Clip_Left(swvert_t *out, swvert_t *in, swvert_t *result)
{
	float frac;
	frac =	(0 - in->vcoord[0]) /
			(out->vcoord[0] - in->vcoord[0]);
	VectorInterpolate(in->vcoord, frac, out->vcoord, result->vcoord);
	result->vcoord[0] = 0;
	Vector2Interpolate(in->tccoord, frac, out->tccoord, result->tccoord);
}
static void WT_Clip_Right(swvert_t *out, swvert_t *in, swvert_t *result)
{
	float frac;
	frac =	(vid.pixelwidth-1 - in->vcoord[0]) /
			(out->vcoord[0] - in->vcoord[0]);
	VectorInterpolate(in->vcoord, frac, out->vcoord, result->vcoord);
	result->vcoord[0] = vid.pixelwidth-1;
	Vector2Interpolate(in->tccoord, frac, out->tccoord, result->tccoord);
}
static void WT_Clip_Near(swvert_t *out, swvert_t *in, swvert_t *result)
{
	extern cvar_t temp1;
	double frac;
	frac =	(temp1.value - in->vcoord[2]) /
			(out->vcoord[2] - in->vcoord[2]);
	VectorInterpolate(in->vcoord, frac, out->vcoord, result->vcoord);
	result->vcoord[2] = temp1.value;
	Vector2Interpolate(in->tccoord, frac, out->tccoord, result->tccoord);
}

static int WT_ClipPoly(int incount, swvert_t *inv, swvert_t *outv, int flag, void (*clip)(swvert_t *out, swvert_t *in, swvert_t *result))
{
	int p, c;
	int result = 0;
	int pf, cf;
	if (incount < 3)
		return 0;

	for (p = incount - 1, c = 0; c < incount; p = c, c++)
	{
		pf = inv[p].clipflags & flag;
		cf = inv[c].clipflags & flag;

		if (pf && cf)
			continue;	//both clipped, skip it now
		if (pf ^ cf)
		{
			//crossed... emit a new vertex on the boundary
			if (cf)	//new is offscreen
				clip(&inv[c], &inv[p], &outv[result]);
			else
				clip(&inv[p], &inv[c], &outv[result]);
			outv[result].clipflags = 0;

			if (outv[result].vcoord[0] < 0)
				outv[result].clipflags = CLIP_LEFT_FLAG;
			if (outv[result].vcoord[0] > vid.pixelwidth-1)
				outv[result].clipflags |= CLIP_RIGHT_FLAG;
			if (outv[result].vcoord[1] < 0)
				outv[result].clipflags |= CLIP_TOP_FLAG;
			if (outv[result].vcoord[1] > vid.pixelheight-1)
				outv[result].clipflags |= CLIP_BOTTOM_FLAG;

			result++;
		}
		if (!cf)
		{
			outv[result] = inv[c];
			result++;
		}
	}
	return result;
}

static void WT_ClipTriangle(swthread_t *th, swimage_t *img, swvert_t *v1, swvert_t *v2, swvert_t *v3)
{
	unsigned int cflags;
	swvert_t final[2][64];
	int list = 0;
	int i;
	int count;

	if (v1->clipflags & v2->clipflags & v3->clipflags)
		return;	//all verticies are off at least one single side
	
	cflags = v1->clipflags | v2->clipflags | v3->clipflags;

	if (!cflags)
	{
		//no clipping to be done
		WT_Triangle(th, img, v1, v2, v3);
		return;
	}

	final[list][0] = *v1;
	final[list][1] = *v2;
	final[list][2] = *v3;
	count = 3;

	if (cflags & CLIP_NEAR_FLAG)
	{
		count = WT_ClipPoly(count, final[list], final[list^1], CLIP_NEAR_FLAG, WT_Clip_Near);
		list ^= 1;
	}
	if (cflags & CLIP_TOP_FLAG)
	{
		count = WT_ClipPoly(count, final[list], final[list^1], CLIP_TOP_FLAG, WT_Clip_Top);
		list ^= 1;
	}
	if (cflags & CLIP_BOTTOM_FLAG)
	{
		count = WT_ClipPoly(count, final[list], final[list^1], CLIP_BOTTOM_FLAG, WT_Clip_Bottom);
		list ^= 1;
	}
	if (cflags & CLIP_LEFT_FLAG)
	{
		count = WT_ClipPoly(count, final[list], final[list^1], CLIP_LEFT_FLAG, WT_Clip_Left);
		list ^= 1;
	}
	if (cflags & CLIP_RIGHT_FLAG)
	{
		count = WT_ClipPoly(count, final[list], final[list^1], CLIP_RIGHT_FLAG, WT_Clip_Right);
		list ^= 1;
	}

	for (i = 2; i < count; i++)
	{
		WT_Triangle(th, img, &final[list][0], &final[list][i-1], &final[list][i]);
	}
}

void WQ_ClearBuffer(swthread_t *t, unsigned int *mbuf, qintptr_t stride, unsigned int clearval)
{
	int y;
	int x;
	unsigned int *buf;

	for (y = t->interlaceline; y < t->vpheight; y += t->interlacemod)
	{
		buf = mbuf + stride*y;
		for (x = 0; x < (t->vpwidth & ~15);)
		{
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
			buf[x++] = clearval;
		}
		for (; x < t->vpwidth; )
			buf[x++] = clearval;
	}
}

qboolean WT_HandleCommand(swthread_t *t, wqcom_t *com)
{
	index_t *idx;
	int i;
	switch(com->com.command)
	{
	case WTC_DIE:
		t->readpoint += com->com.cmdsize;
		return 1;
	case WTC_NOOP:
		break;
	case WTC_NEWFRAME:
		break;
	case WTC_VIEWPORT:
		t->vpcbuf = com->viewport.cbuf;
		t->vpdbuf = com->viewport.dbuf;
		t->vpwidth = com->viewport.width;
		t->vpheight = com->viewport.height;
		t->vpcstride = com->viewport.stride;
		if (!t->wq->numthreads)
		{
			t->interlacemod = com->viewport.interlace;	//this many vthreads
			t->interlaceline = com->viewport.framenum%com->viewport.interlace;	//this vthread
		}
		else
		{
			t->interlacemod = t->wq->numthreads*com->viewport.interlace;	//this many vthreads
			t->interlaceline = (t->threadnum*com->viewport.interlace) + (com->viewport.framenum%com->viewport.interlace);	//this vthread
		}

		if (com->viewport.clearcolour)
		{
			WQ_ClearBuffer(t, t->vpcbuf, t->vpcstride, 0);
		}
		if (com->viewport.cleardepth)
		{
			WQ_ClearBuffer(t, t->vpdbuf, t->vpwidth, ~0u);
		}
		break;
	case WTC_TRIFAN:
		for (i = 2; i < com->trifan.numverts; i++)
		{
			WT_ClipTriangle(t, com->trifan.texture, &com->trifan.verts[0], &com->trifan.verts[i-1], &com->trifan.verts[i]);
		}
		break;
	case WTC_TRISOUP:
		idx = (index_t*)(com->trisoup.verts + com->trisoup.numverts);
		for (i = 0; i < com->trisoup.numidx; i+=3, idx+=3)
		{
			WT_ClipTriangle(t, com->trisoup.texture, &com->trisoup.verts[idx[0]], &com->trisoup.verts[idx[1]], &com->trisoup.verts[idx[2]]);
		}
		break;
	case WTC_SPANS:
		break;
	default:
		Sys_Printf("Unknown render command!\n");
		break;
	}
	t->readpoint += com->com.cmdsize;
	return false;
}

int WT_Main(void *ptr)
{
	wqcom_t *com;
	swthread_t *t = ptr;
	for(;;)
	{
		if (t->readpoint == t->wq->pos)
		{
			Sys_Sleep(0);
			continue;
		}
		com = (wqcom_t*)&t->wq->queue[t->readpoint & WQ_MASK];
		if (WT_HandleCommand(t, com))
			break;
	}
	return 0;
}
void SWRast_EndCommand(struct workqueue_s *wq, wqcom_t *com)
{
	wq->pos += com->com.cmdsize;

	if (!wq->numthreads)
	{
		//immediate mode
		WT_HandleCommand(wq->swthreads, com);
	}
}
wqcom_t *SWRast_BeginCommand(struct workqueue_s *wq, int cmdtype, unsigned int size)
{
	wqcom_t *com;
	//round the command size up, so we always have space for a noop/wrap if needed
	size = (size + sizeof(com->align)) & ~(sizeof(com->align)-1);

	//generate a noop if we don't have enough space for the command
	if ((wq->pos&WQ_MASK) + size > WQ_SIZE)
	{
//		SWRast_Sync();
		com = (wqcom_t *)&wq->queue[wq->pos&WQ_MASK];
		com->com.cmdsize = WQ_SIZE - wq->pos&WQ_MASK;
		com->com.command = WTC_NOOP;
		SWRast_EndCommand(wq, com);
	}

	com = (wqcom_t *)&wq->queue[wq->pos&WQ_MASK];
	com->com.cmdsize = size;
	com->com.command = cmdtype;

	return com;
}
void SWRast_Sync(struct workqueue_s *wq)
{
	int i;
	swthread_t *t;

	for (i = 0; i < wq->numthreads; i++)
	{
		t = &wq->swthreads[i];
		while (t->readpoint != wq->pos)
			;
	}

	//all worker threads are up to speed
}
void SWRast_CreateThreadPool(struct workqueue_s *wq, int numthreads)
{
	int i = 0;
	swthread_t *t;
	wq->pos = 0;
	numthreads = ((numthreads > WQ_MAXTHREADS)?WQ_MAXTHREADS:numthreads);
#ifdef MULTITHREAD
	for (i = 0; i < numthreads; i++)
	{
		t = &wq->swthreads[i];
		t->threadnum = i;
		t->thread = Sys_CreateThread("swrast", WT_Main, t, THREADP_NORMAL, 0);
		if (!t->thread)
			break;
	}
#else
	numthreads = 0;
#endif
	wq->numthreads = i;

	if (i == 0)
		numthreads = 1;
	else
		numthreads = i;
	for (i = 0; i < numthreads; i++)
	{
		wq->swthreads[i].readpoint = wq->pos;
		wq->swthreads[i].wq = wq;
	}
}
void SWRast_TerminateThreadPool(struct workqueue_s *wq)
{
	int i;
	wqcom_t *com = SWRast_BeginCommand(wq, WTC_DIE, sizeof(com->com));
	SWRast_EndCommand(wq, com);
#ifdef MULTITHREAD
	for (i = 0; i < wq->numthreads; i++)
	{
		Sys_WaitOnThread(wq->swthreads[i].thread);
	}
#endif
	wq->numthreads = 0;
}













void SW_Draw_Init(void)
{
	R2D_Init();

	R_InitFlashblends();
}
void SW_Draw_Shutdown(void)
{
	R2D_Shutdown();
}
void SW_R_Init(void)
{
	SWRast_CreateThreadPool(&commandqueue, sw_vthread.ival?1:0);
	sw_vthread.modified = true;
}
void SW_R_DeInit(void)
{
	SWRast_TerminateThreadPool(&commandqueue);
}
void SW_R_RenderView(void)
{
	extern cvar_t gl_screenangle;
	extern cvar_t gl_mindist;
	vec3_t newa;
	int tmpvisents = cl_numvisedicts;	/*world rendering is allowed to add additional ents, but we don't want to keep them for recursive views*/
	if (!cl.worldmodel || (!cl.worldmodel->nodes && cl.worldmodel->type != mod_heightmap))
		r_refdef.flags |= Q2RDF_NOWORLDMODEL;

//	R_SetupGL ();

	AngleVectors (r_refdef.viewangles, vpn, vright, vup);
	VectorCopy (r_refdef.vieworg, r_origin);
	if (r_refdef.useperspective)
		Matrix4x4_CM_Projection_Inf(r_refdef.m_projection, r_refdef.fov_x, r_refdef.fov_y, gl_mindist.value);
	else
	{
		if (gl_maxdist.value>=1)
			Matrix4x4_CM_Orthographic(r_refdef.m_projection, -r_refdef.fov_x/2, r_refdef.fov_x/2, -r_refdef.fov_y/2, r_refdef.fov_y/2, -gl_maxdist.value, gl_maxdist.value);
		else
			Matrix4x4_CM_Orthographic(r_refdef.m_projection, 0, r_refdef.vrect.width, 0, r_refdef.vrect.height, -9999, 9999);
	}
	VectorCopy(r_refdef.viewangles, newa);
	newa[0] = r_refdef.viewangles[0];
	newa[1] = r_refdef.viewangles[1];
	newa[2] = r_refdef.viewangles[2] + gl_screenangle.value;
	Matrix4x4_CM_ModelViewMatrix(r_refdef.m_view, newa, r_refdef.vieworg);

	R_SetFrustum (r_refdef.m_projection, r_refdef.m_view);

	RQ_BeginFrame();

	Surf_DrawWorld ();		// adds static entities to the list

	S_ExtraUpdate ();	// don't let sound get messed up if going slow

//	R_DrawDecals();

	R_RenderDlights ();

	RQ_RenderBatchClear();

	cl_numvisedicts = tmpvisents;
}
void SW_R_NewMap(void)
{
	char namebuf[MAX_QPATH];
	extern cvar_t host_mapname, r_shadow_realtime_dlight, r_shadow_realtime_world;
	int		i;
	
	for (i=0 ; i<256 ; i++)
		d_lightstylevalue[i] = 264;		// normal light value

	memset (&r_worldentity, 0, sizeof(r_worldentity));
	AngleVectors(r_worldentity.angles, r_worldentity.axis[0], r_worldentity.axis[1], r_worldentity.axis[2]);
	VectorInverse(r_worldentity.axis[1]);
	r_worldentity.model = cl.worldmodel;
	Vector4Set(r_worldentity.shaderRGBAf, 1, 1, 1, 1);


	COM_StripExtension(COM_SkipPath(cl.worldmodel->name), namebuf, sizeof(namebuf));
	Cvar_Set(&host_mapname, namebuf);

	Surf_DeInit();

	r_viewleaf = NULL;
	r_viewcluster = -1;
	r_oldviewcluster = 0;
	r_viewcluster2 = -1;

	Mod_ParseInfoFromEntityLump(cl.worldmodel, cl.worldmodel->entities, cl.worldmodel->name);

	P_ClearParticles ();
	Surf_WipeStains();
	CL_RegisterParticles();
	Surf_BuildLightmaps ();


#ifdef VM_UI
	UI_Reset();
#endif

	TP_NewMap();
	R_SetSky(cl.skyname);

#ifdef MAP_PROC
	if (cl.worldmodel->fromgame == fg_doom3)
		D3_GenerateAreas(cl.worldmodel);
#endif

#ifdef RTLIGHTS
	if (r_shadow_realtime_dlight.ival || r_shadow_realtime_world.ival)
	{
		R_LoadRTLights();
		if (rtlights_first == rtlights_max)
			R_ImportRTLights(cl.worldmodel->entities);
	}
	Sh_PreGenerateLights();
#endif
}
void SW_R_PreNewMap(void)
{
}
void SW_SCR_UpdateScreen(void)
{
	wqcom_t *com;

	extern cvar_t gl_screenangle;
	float w = vid.width, h = vid.height;

	r_refdef.time = realtime;

	SWBE_Set2D();

	SWRast_Sync(&commandqueue);
	SWRast_Sync(&spanqueue);
	SW_VID_SwapBuffers();
	if (sw_vthread.modified)
	{
		SWRast_TerminateThreadPool(&commandqueue);
		SWRast_CreateThreadPool(&commandqueue, sw_vthread.ival?1:0);
		sw_vthread.modified = false;
	}
	if (sw_fthreads.modified)
	{
		SWRast_TerminateThreadPool(&spanqueue);
		SWRast_CreateThreadPool(&spanqueue, sw_fthreads.ival);
		sw_fthreads.modified = false;
	}

	com = SWRast_BeginCommand(&commandqueue, WTC_VIEWPORT, sizeof(com->viewport));
	com->viewport.interlace = bound(0, sw_interlace.ival, 15)+1;
	com->viewport.clearcolour = r_clear.ival;
	com->viewport.cleardepth = true;
	SW_VID_UpdateViewport(com);
	SWRast_EndCommand(&commandqueue, com);

	Shader_DoReload();

	//FIXME: playfilm/editor+q3ui
	if (vid.recalc_refdef)
		SCR_CalcRefdef ();
	SCR_SetUpToDrawConsole ();

	if (cls.state == ca_active)
	{
		if (!CSQC_DrawView())
			V_RenderView ();

		R2D_PolyBlend ();
		R2D_BrightenScreen();
	}

	SCR_DrawTwoDimensional(0, 0);

	V_UpdatePalette (false);
}

rendererinfo_t swrendererinfo =
{
	"Software Renderer",
	{
		"sw",
		"Software",
		"SoftRast",
	},
	QR_SOFTWARE,

	SW_Draw_Init,
	SW_Draw_Shutdown,

	SW_LoadTexture,
	SW_LoadTexture8Pal24,
	SW_LoadTexture8Pal32,
	SW_LoadCompressed,
	SW_FindTexture,
	SW_AllocNewTexture,
	SW_Upload,
	SW_DestroyTexture,

	SW_R_Init,
	SW_R_DeInit,
	SW_R_RenderView,

	SW_R_NewMap,
	SW_R_PreNewMap,

	Surf_AddStain,
	Surf_LessenStains,

	RMod_Init,
	RMod_Shutdown,
	RMod_ClearAll,
	RMod_ForName,
	RMod_FindName,
	RMod_Extradata,
	RMod_TouchModel,

	RMod_NowLoadExternal,
	RMod_Think,
	Mod_GetTag,
	Mod_TagNumForName,
	Mod_SkinNumForName,
	Mod_FrameNumForName,
	Mod_FrameDuration,


	SW_VID_Init,
	SW_VID_DeInit,
	SW_VID_SetPalette,
	SW_VID_ShiftPalette,
	SW_VID_GetRGBInfo,
	SW_VID_SetWindowCaption,

	SW_SCR_UpdateScreen,

	SWBE_SelectMode,
	SWBE_DrawMesh_List,
	SWBE_DrawMesh_Single,
	SWBE_SubmitBatch,
	SWBE_GetTempBatch,
	SWBE_DrawWorld,
	SWBE_Init,
	SWBE_GenBrushModelVBO,
	SWBE_ClearVBO,
	SWBE_UploadAllLightmaps,
	SWBE_SelectEntity,
	SWBE_SelectDLight,
	SWBE_LightCullModel,

	"no more"
};
#endif
