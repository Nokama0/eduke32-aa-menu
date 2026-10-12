# Model bounding boxes: raw MD3 numbers to Build coordinates (modelbounds.h, ekm_mdl_scale_bbox)

The menu boxes a model sprite from the extents the engine loaded. This note derives the transform.

## Inputs
- The inputs are the RAW numbers md3postload_common (mdsprite.cpp:1516-1583) leaves in the frame. It discards the
  file's box and rebuilds min/max from md3xyzn_t vertices, in [10:6] fixed point. The fourcc is 'IDP3'
  (build/include/mdsprite.h:30).
- modelid range: build.h:1619 tile2model_t.modelid, mdsprite.h:231 nextmodelid (one past the loaded range).
- bscale is the def file's number: md_setmisc (mdsprite.cpp:279) writes the declared scale to m->bscale and leaves
  m->scale at the loader's 0.01f.

## Both renderers reduce to the same Build coordinates (repeats excluded)
- Polymost: scale/64 per raw vertex (mdsprite.cpp:2028), then xrepeat*(1/64)*bscale (:2052); md3_vox_calcmat_common adds
  the sprite offset at 1/1024.
- Polymer never divides by 64: polymer.cpp:4475 and :4532 use scale*0.25f*bscale against surfs[].geometry, which
  mdsprite.cpp:1722-1724 fills with the raw int16.
- Result: x,y = raw*scale*bscale*0.25; z = -raw*scale*bscale*4 - (zadd+yoffset)*scale*bscale*256. Build's z is 16x x/y
  (its vertical unit), and the offset is 64x z's (the [10:6] storage), which confirms the derivation.
- The 1/64 is already inside the 0.25 and the 4. x,y are the model's own extents (which build axis depends on
  tspr->ang).

## z runs the other way
Build z is down-positive. polymer.cpp:4458 has build_z = -16*GL_y; glRotatef(-90,1,0,0) at :4508 makes the model's third
component vertical; :4514 / :4531 / :4532 give build_z = -4*s*yrepeat*v_z - 256*s*yrepeat*(zadd+yoffset). Polymost lands
the same via mdsprite.cpp:2076 (the WORLD term carries the minus). An MD3 stands on its origin, so raw 0..+H is build
-4sH..0, ABOVE the sprite. Keeping the three magnitudes but not swapping min and max gives a z-up box.

## Method
Engine source reading (mdsprite.cpp, polymer.cpp).
