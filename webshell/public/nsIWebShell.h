/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
#ifndef nsIWebShell_h___
#define nsIWebShell_h___

#include "nsIContentViewerContainer.h"

class nsIDocumentLoader;
class nsIURI;

// Interface ID for nsIWebShell
// e1c77239-eb80-42bb-8b94-54b49fe1b25b
#define NS_IWEB_SHELL_IID \
 { 0xe1c77239, 0xeb80, 0x42bb,{0x8b, 0x94, 0x54, 0xb4, 0x9f, 0xe1, 0xb2, 0x5b}}

// Interface ID for nsIWebShellContainer
#define NS_IWEB_SHELL_CONTAINER_IID \
 { 0xa6cf905a, 0x15b3, 0x11d2,{0x93, 0x2e, 0x00, 0x80, 0x5f, 0x8a, 0xdd, 0x32}}

// Class ID for an implementation of nsIWebShell
#define NS_WEB_SHELL_CID \
 { 0xa6cf9059, 0x15b3, 0x11d2,{0x93, 0x2e, 0x00, 0x80, 0x5f, 0x8a, 0xdd, 0x32}}

//----------------------------------------------------------------------

typedef enum {
  nsLoadURL,
  nsLoadHistory,
  nsLoadLink,
  nsLoadRefresh
} nsLoadType;

// Container for web shell's
class nsIWebShellContainer : public nsISupports {
public:
  NS_DEFINE_STATIC_IID_ACCESSOR(NS_IWEB_SHELL_CONTAINER_IID)

};

//----------------------------------------------------------------------

/**
 * The web shell is a container for implementations of nsIContentViewer.
 * It is a content-viewer-container and also knows how to delegate certain
 * behavior to an nsIWebShellContainer.
 *
 * Web shells can be arranged in a tree.
 *
 * Web shells are also nsIWebShellContainer's because they can contain
 * other web shells.
 */
class nsIWebShell : public nsISupports {
public:
  NS_DEFINE_STATIC_IID_ACCESSOR(NS_IWEB_SHELL_IID)

  /**
   * WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING !!!!
   *
   * THIS INTERFACE IS DEPRECATED. DO NOT ADD STUFF OR CODE TO IT!!!!
   */

  // XXXbz there is no need for this interface anymore... Callers can either
  // just get the parent treeitem, or when they want the container of the root
  // docshell can get the treeowner and GetInterface nsIXULWindow to get what
  // GetContainer currently returns...

  // XXXbz oh, and the Show() method on nsIWebShellWindow is just redundant
  // with SetVisibility on nsIBaseWindow....  Mailnews uses this interface for
  // that stuff, but it doesn't have to.
    
  /**
   * Set the nsIWebShellContainer for the WebShell.
   */
  NS_IMETHOD SetContainer(nsIWebShellContainer* aContainer) = 0;

  /**
   * Return the current nsIWebShellContainer.
   */
  NS_IMETHOD GetContainer(nsIWebShellContainer*& aResult) = 0;
};

#endif /* nsIWebShell_h___ */
