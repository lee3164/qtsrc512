// Copyright 2017 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef FXJS_CJS_GLOBALARRAYS_H_
#define FXJS_CJS_GLOBALARRAYS_H_

#include "fxjs/js_define.h"

class CJS_GlobalArrays : public CJS_Object {
 public:
  static void DefineJSObjects(CJS_Runtime* pRuntmie);
};

#endif  // FXJS_CJS_GLOBALARRAYS_H_
