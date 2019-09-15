/*
 * (C) 2003-2006 Gabest
 * (C) 2006-2018 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <memory>
#include <string>

#include "stdafx.h"
#include <afxdlgs.h>
#include <atlpath.h>
#include "resource.h"
#include "../../../Subtitles/VobSubFile.h"
#include "../../../Subtitles/RTS.h"
#include "../../../SubPic/MemSubPic.h"
#include "../../../SubPic/SubPicQueueImpl.h"
#include "../../../DSUtil/WinAPIUtils.h"
#include "vfr.h"

#pragma warning(disable: 4706)

// Size of the char buffer according to VirtualDub Filters SDK doc
#define STRING_PROC_BUFFER_SIZE 128

//
// Generic interface
//

namespace Plugin
{

	class CFilter : public CAMThread, public CCritSec
	{
	private:
		CString m_fn;

	protected:
		float m_fps;
		CCritSec m_csSubLock;
		CComPtr<ISubPicQueue> m_pSubPicQueue;
		CComPtr<ISubPicProvider> m_pSubPicProvider;
		DWORD_PTR m_SubPicProviderId;

	public:
		CFilter() : m_fps(-1), m_SubPicProviderId(0) {
			CAMThread::Create();
		}
		virtual ~CFilter() {
			CAMThread::CallWorker(0);
		}

		CString GetFileName() {
			CAutoLock cAutoLock(this);
			return m_fn;
		}
		void SetFileName(CString fn) {
			CAutoLock cAutoLock(this);
			m_fn = fn;
		}

		bool Render(SubPicDesc& dst, REFERENCE_TIME rt, float fps) {
			if (!m_pSubPicProvider) {
				return false;
			}

			CSize size(dst.w, dst.h);

			if (!m_pSubPicQueue) {
				CComPtr<ISubPicAllocator> pAllocator = DNew CMemSubPicAllocator(dst.type, size);

				HRESULT hr;
				if (!(m_pSubPicQueue = DNew CSubPicQueueNoThread(false, pAllocator, &hr)) || FAILED(hr)) {
					m_pSubPicQueue = nullptr;
					return false;
				}
			}

			if (m_SubPicProviderId != (DWORD_PTR)(ISubPicProvider*)m_pSubPicProvider) {
				m_pSubPicQueue->SetSubPicProvider(m_pSubPicProvider);
				m_SubPicProviderId = (DWORD_PTR)(ISubPicProvider*)m_pSubPicProvider;
			}

			CComPtr<ISubPic> pSubPic;
			if (!m_pSubPicQueue->LookupSubPic(rt, pSubPic)) {
				return false;
			}

			CRect r;
			pSubPic->GetDirtyRect(r);

			if (dst.type == MSP_RGB32 || dst.type == MSP_RGB24 || dst.type == MSP_RGB16 || dst.type == MSP_RGB15) {
				dst.h = -dst.h;
			}

			pSubPic->AlphaBlt(r, r, &dst);

			return true;
		}

		DWORD ThreadProc() {
			SetThreadPriority(m_hThread, THREAD_PRIORITY_LOWEST);

			std::vector<HANDLE> handles;
			handles.push_back(GetRequestHandle());

			CString fn = GetFileName();
			CFileStatus fs;
			fs.m_mtime = 0;
			CFileGetStatus(fn, fs);

			for (;;) {
				DWORD i = WaitForMultipleObjects(handles.size(), handles.data(), FALSE, 1000);

				if (WAIT_OBJECT_0 == i) {
					Reply(S_OK);
					break;
				} else if (WAIT_OBJECT_0 + 1 >= i && i <= WAIT_OBJECT_0 + handles.size()) {
					if (FindNextChangeNotification(handles[i - WAIT_OBJECT_0])) {
						CFileStatus fs2;
						fs2.m_mtime = 0;
						CFileGetStatus(fn, fs2);

						if (fs.m_mtime < fs2.m_mtime) {
							fs.m_mtime = fs2.m_mtime;

							if (CComQIPtr<ISubStream> pSubStream = m_pSubPicProvider) {
								CAutoLock cAutoLock(&m_csSubLock);
								pSubStream->Reload();
							}
						}
					}
				} else if (WAIT_TIMEOUT == i) {
					CString fn2 = GetFileName();

					if (fn != fn2) {
						CPath p(fn2);
						p.RemoveFileSpec();
						HANDLE h = FindFirstChangeNotificationW(p, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
						if (h != INVALID_HANDLE_VALUE) {
							fn = fn2;
							handles.resize(1);
							handles.push_back(h);
						}
					}
				} else { // if(WAIT_ABANDONED_0 == i || WAIT_FAILED == i)
					break;
				}
			}

			m_hThread = 0;

			for (size_t i = 1; i < handles.size(); i++) {
				FindCloseChangeNotification(handles[i]);
			}

			return 0;
		}
	};

	class CVobSubFilter : virtual public CFilter
	{
	public:
		CVobSubFilter(CString fn = L"") {
			if (!fn.IsEmpty()) {
				Open(fn);
			}
		}

		bool Open(CString fn) {
			SetFileName(L"");
			m_pSubPicProvider = nullptr;

			if (CVobSubFile* vsf = DNew CVobSubFile(&m_csSubLock)) {
				m_pSubPicProvider = (ISubPicProvider*)vsf;
				if (vsf->Open(CString(fn))) {
					SetFileName(fn);
				} else {
					m_pSubPicProvider = nullptr;
				}
			}

			return !!m_pSubPicProvider;
		}
	};

	class CTextSubFilter : virtual public CFilter
	{
		int m_CharSet;

	public:
		CTextSubFilter(CString fn = L"", int CharSet = DEFAULT_CHARSET, float fps = -1)
			: m_CharSet(CharSet) {
			m_fps = fps;
			if (!fn.IsEmpty()) {
				Open(fn, CharSet);
			}
		}

		int GetCharSet() {
			return(m_CharSet);
		}

		bool Open(CString fn, int CharSet = DEFAULT_CHARSET) {
			SetFileName(L"");
			m_pSubPicProvider = nullptr;

			if (!m_pSubPicProvider) {
				if (CRenderedTextSubtitle* rts = DNew CRenderedTextSubtitle(&m_csSubLock)) {
					m_pSubPicProvider = (ISubPicProvider*)rts;
					if (rts->Open(CString(fn), CharSet)) {
						SetFileName(fn);
					} else {
						m_pSubPicProvider = nullptr;
					}
				}
			}

			return !!m_pSubPicProvider;
		}
	};

#ifndef _WIN64
	//
	// old VirtualDub interface
	//

	namespace VirtualDub
	{
#include <vd2/extras/FilterSDK/VirtualDub.h>

		class CVirtualDubFilter : virtual public CFilter
		{
		public:
			CVirtualDubFilter() {}
			virtual ~CVirtualDubFilter() {}

			virtual int RunProc(const FilterActivation* fa, const FilterFunctions* ff) {
				SubPicDesc dst;
				dst.type = MSP_RGB32;
				dst.w = fa->src.w;
				dst.h = fa->src.h;
				dst.bpp = 32;
				dst.pitch = fa->src.pitch;
				dst.bits = (LPVOID)fa->src.data;

				Render(dst, 10000i64*fa->pfsi->lSourceFrameMS, (float)1000 / fa->pfsi->lMicrosecsPerFrame);

				return 0;
			}

			virtual long ParamProc(FilterActivation* fa, const FilterFunctions* ff) {
				fa->dst.offset	= fa->src.offset;
				fa->dst.modulo	= fa->src.modulo;
				fa->dst.pitch	= fa->src.pitch;

				return 0;
			}

			virtual int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) = 0;
			virtual void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) = 0;
			virtual bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) = 0;
		};

		class CVobSubVirtualDubFilter : public CVobSubFilter, public CVirtualDubFilter
		{
		public:
			CVobSubVirtualDubFilter(CString fn = L"")
				: CVobSubFilter(fn) {}

			int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) {
				AFX_MANAGE_STATE(AfxGetStaticModuleState());

				CFileDialog fd(TRUE, nullptr, GetFileName(), OFN_EXPLORER|OFN_ENABLESIZING|OFN_HIDEREADONLY,
							   L"VobSub files (*.idx;*.sub)|*.idx;*.sub||", CWnd::FromHandle(hwnd), 0);

				if (fd.DoModal() != IDOK) {
					return 1;
				}

				return Open(fd.GetPathName()) ? 0 : 1;
			}

			void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) {
				sprintf_s(str, STRING_PROC_BUFFER_SIZE, " (%s)", GetFileName().IsEmpty() ? " (empty)" : CStringA(GetFileName()).GetString());
			}

			bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) {
				CStringA fn(GetFileName());
				fn.Replace("\\", "\\\\");
				_snprintf_s(buf, buflen, buflen, "Config(\"%s\")", fn);
				return true;
			}
		};

		class CTextSubVirtualDubFilter : public CTextSubFilter, public CVirtualDubFilter
		{
		public:
			CTextSubVirtualDubFilter(CString fn = L"", int CharSet = DEFAULT_CHARSET)
				: CTextSubFilter(fn, CharSet) {}

			int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) {
				AFX_MANAGE_STATE(AfxGetStaticModuleState());

				const WCHAR formats[] = L"TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||";
				CFileDialog fd(TRUE, nullptr, GetFileName(), OFN_EXPLORER|OFN_ENABLESIZING|OFN_HIDEREADONLY|OFN_ENABLETEMPLATE|OFN_ENABLEHOOK,
							   formats, CWnd::FromHandle(hwnd), sizeof(OPENFILENAME));
				UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam);

				fd.m_pOFN->hInstance = AfxGetResourceHandle();
				fd.m_pOFN->lpTemplateName = MAKEINTRESOURCEW(IDD_TEXTSUBOPENTEMPLATE);
				fd.m_pOFN->lpfnHook = (LPOFNHOOKPROC)OpenHookProc;
				fd.m_pOFN->lCustData = (LPARAM)DEFAULT_CHARSET;

				if (fd.DoModal() != IDOK) {
					return 1;
				}

				return Open(fd.GetPathName(), fd.m_pOFN->lCustData) ? 0 : 1;
			}

			void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) {
				if (!GetFileName().IsEmpty()) {
					sprintf_s(str, STRING_PROC_BUFFER_SIZE, " (%s, %d)", CStringA(GetFileName()).GetString(), GetCharSet());
				} else {
					sprintf_s(str, STRING_PROC_BUFFER_SIZE, " (empty)");
				}
			}

			bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) {
				CStringA fn(GetFileName());
				fn.Replace("\\", "\\\\");
				_snprintf_s(buf, buflen, buflen, "Config(\"%s\", %d)", fn, GetCharSet());
				return true;
			}
		};

		int vobsubInitProc(FilterActivation* fa, const FilterFunctions* ff)
		{
			*(CVirtualDubFilter**)fa->filter_data = DNew CVobSubVirtualDubFilter();
			return !(*(CVirtualDubFilter**)fa->filter_data);
		}

		int textsubInitProc(FilterActivation* fa, const FilterFunctions* ff)
		{
			*(CVirtualDubFilter**)fa->filter_data = DNew CTextSubVirtualDubFilter();
			return !(*(CVirtualDubFilter**)fa->filter_data);
		}

		void baseDeinitProc(FilterActivation* fa, const FilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f, f = nullptr;
			}
		}

		int baseRunProc(const FilterActivation* fa, const FilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->RunProc(fa, ff) : 1;
		}

		long baseParamProc(FilterActivation* fa, const FilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->ParamProc(fa, ff) : 1;
		}

		int baseConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->ConfigProc(fa, ff, hwnd) : 1;
		}

		void baseStringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				f->StringProc(fa, ff, str);
			}
		}

		bool baseFssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->FssProc(fa, ff, buf, buflen) : false;
		}

		void vobsubScriptConfig(IScriptInterpreter* isi, void* lpVoid, CScriptValue* argv, int argc)
		{
			FilterActivation* fa = (FilterActivation*)lpVoid;
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f;
			}
			f = DNew CVobSubVirtualDubFilter(CString(*argv[0].asString()));
			*(CVirtualDubFilter**)fa->filter_data = f;
		}

		void textsubScriptConfig(IScriptInterpreter* isi, void* lpVoid, CScriptValue* argv, int argc)
		{
			FilterActivation* fa = (FilterActivation*)lpVoid;
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f;
			}
			f = DNew CTextSubVirtualDubFilter(CString(*argv[0].asString()), argv[1].asInt());
			*(CVirtualDubFilter**)fa->filter_data = f;
		}

		ScriptFunctionDef vobsub_func_defs[]= {
			{ (ScriptFunctionPtr)vobsubScriptConfig, "Config", "0s" },
			{ nullptr },
		};

		CScriptObject vobsub_obj= {
			nullptr, vobsub_func_defs
		};

		struct FilterDefinition filterDef_vobsub = {
			nullptr, nullptr, nullptr,	// next, prev, module
			"VobSub",					// name
			"Adds subtitles from a vob sequence.", // desc
			"Gabest",					// maker
			nullptr,					// private_data
			sizeof(CVirtualDubFilter**), // inst_data_size
			vobsubInitProc,				// initProc
			baseDeinitProc,				// deinitProc
			baseRunProc,				// runProc
			baseParamProc,				// paramProc
			baseConfigProc,				// configProc
			baseStringProc,				// stringProc
			nullptr,					// startProc
			nullptr,					// endProc
			&vobsub_obj,				// script_obj
			baseFssProc,				// fssProc
		};

		ScriptFunctionDef textsub_func_defs[]= {
			{ (ScriptFunctionPtr)textsubScriptConfig, "Config", "0si" },
			{ nullptr },
		};

		CScriptObject textsub_obj= {
			nullptr, textsub_func_defs
		};

		struct FilterDefinition filterDef_textsub = {
			nullptr, nullptr, nullptr,	// next, prev, module
			"TextSub",					// name
			"Adds subtitles from srt, sub, psb, smi, ssa, ass file formats.", // desc
			"Gabest",					// maker
			nullptr,					// private_data
			sizeof(CVirtualDubFilter**), // inst_data_size
			textsubInitProc,			// initProc
			baseDeinitProc,				// deinitProc
			baseRunProc,				// runProc
			baseParamProc,				// paramProc
			baseConfigProc,				// configProc
			baseStringProc,				// stringProc
			nullptr,					// startProc
			nullptr,					// endProc
			&textsub_obj,				// script_obj
			baseFssProc,				// fssProc
		};

		static FilterDefinition* fd_vobsub;
		static FilterDefinition* fd_textsub;

		extern "C" __declspec(dllexport) int __cdecl VirtualdubFilterModuleInit2(FilterModule *fm, const FilterFunctions *ff, int& vdfd_ver, int& vdfd_compat)
		{
			fd_vobsub = ff->addFilter(fm, &filterDef_vobsub, sizeof(FilterDefinition));
			if (!fd_vobsub) {
				return 1;
			}
			fd_textsub = ff->addFilter(fm, &filterDef_textsub, sizeof(FilterDefinition));
			if (!fd_textsub) {
				return 1;
			}

			vdfd_ver = VIRTUALDUB_FILTERDEF_VERSION;
			vdfd_compat = VIRTUALDUB_FILTERDEF_COMPATIBLE;

			return 0;
		}

		extern "C" __declspec(dllexport) void __cdecl VirtualdubFilterModuleDeinit(FilterModule *fm, const FilterFunctions *ff)
		{
			ff->removeFilter(fd_textsub);
			ff->removeFilter(fd_vobsub);
		}
	}/**/

#else
	//
	// VirtualDub new plugin interface sdk 1.1
	//
	namespace VirtualDubNew
	{
#include <vd2/plugin/vdplugin.h>
#include <vd2/plugin/vdvideofilt.h>

		class CVirtualDubFilter : virtual public CFilter
		{
		public:
			CVirtualDubFilter() {}
			virtual ~CVirtualDubFilter() {}

			virtual int RunProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff) {
				SubPicDesc dst;
				dst.type = MSP_RGB32;
				dst.w = fa->src.w;
				dst.h = fa->src.h;
				dst.bpp = 32;
				dst.pitch = fa->src.pitch;
				dst.bits = (LPVOID)fa->src.data;

				Render(dst, 10000i64*fa->pfsi->lSourceFrameMS, (float)1000 / fa->pfsi->lMicrosecsPerFrame);

				return 0;
			}

			virtual long ParamProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff) {
				fa->dst.offset	= fa->src.offset;
				fa->dst.modulo	= fa->src.modulo;
				fa->dst.pitch	= fa->src.pitch;

				return 0;
			}

			virtual int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) = 0;
			virtual void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) = 0;
			virtual bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) = 0;
		};

		class CVobSubVirtualDubFilter : public CVobSubFilter, public CVirtualDubFilter
		{
		public:
			CVobSubVirtualDubFilter(CString fn = L"")
				: CVobSubFilter(fn) {}

			int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) {
				AFX_MANAGE_STATE(AfxGetStaticModuleState());

				CFileDialog fd(TRUE, nullptr, GetFileName(), OFN_EXPLORER|OFN_ENABLESIZING|OFN_HIDEREADONLY,
							   L"VobSub files (*.idx;*.sub)|*.idx;*.sub||", CWnd::FromHandle((HWND)hwnd), 0);

				if (fd.DoModal() != IDOK) {
					return 1;
				}

				return Open(fd.GetPathName()) ? 0 : 1;
			}

			void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) {
				sprintf(str, " (%s)", !GetFileName().IsEmpty() ? CStringA(GetFileName()) : " (empty)");
			}

			bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) {
				CStringA fn(GetFileName());
				fn.Replace("\\", "\\\\");
				_snprintf_s(buf, buflen, buflen, "Config(\"%s\")", fn);
				return true;
			}
		};

		class CTextSubVirtualDubFilter : public CTextSubFilter, public CVirtualDubFilter
		{
		public:
			CTextSubVirtualDubFilter(CString fn = L"", int CharSet = DEFAULT_CHARSET)
				: CTextSubFilter(fn, CharSet) {}

			int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) {
				AFX_MANAGE_STATE(AfxGetStaticModuleState());

				/* off encoding changing */
#ifndef _DEBUG
				const WCHAR formats[] = L"TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||";
				CFileDialog fd(TRUE, nullptr, GetFileName(), OFN_EXPLORER|OFN_ENABLESIZING|OFN_HIDEREADONLY|OFN_ENABLETEMPLATE|OFN_ENABLEHOOK,
							   formats, CWnd::FromHandle((HWND)hwnd), sizeof(OPENFILENAME));
				UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam);

				fd.m_pOFN->hInstance = AfxGetResourceHandle();
				fd.m_pOFN->lpTemplateName = MAKEINTRESOURCEW(IDD_TEXTSUBOPENTEMPLATE);
				fd.m_pOFN->lpfnHook = (LPOFNHOOKPROC)OpenHookProc;
				fd.m_pOFN->lCustData = (LPARAM)DEFAULT_CHARSET;
#else
				const WCHAR formats[] = L"TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||";
				CFileDialog fd(TRUE, nullptr, GetFileName(), OFN_ENABLESIZING|OFN_HIDEREADONLY,
							   formats, CWnd::FromHandle((HWND)hwnd), sizeof(OPENFILENAME));
#endif
				if (fd.DoModal() != IDOK) {
					return 1;
				}

				return Open(fd.GetPathName(), fd.m_pOFN->lCustData) ? 0 : 1;
			}

			void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) {
				if (!GetFileName().IsEmpty()) {
					sprintf(str, " (%s, %d)", CStringA(GetFileName()), GetCharSet());
				} else {
					sprintf(str, " (empty)");
				}
			}

			bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) {
				CStringA fn(GetFileName());
				fn.Replace("\\", "\\\\");
				_snprintf_s(buf, buflen, buflen, "Config(\"%s\", %d)", fn, GetCharSet());
				return true;
			}
		};

		int vobsubInitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
		{
			return !(*(CVirtualDubFilter**)fa->filter_data = DNew CVobSubVirtualDubFilter());
		}

		int textsubInitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
		{
			return !(*(CVirtualDubFilter**)fa->filter_data = DNew CTextSubVirtualDubFilter());
		}

		void baseDeinitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f, f = nullptr;
			}
		}

		int baseRunProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->RunProc(fa, ff) : 1;
		}

		long baseParamProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->ParamProc(fa, ff) : 1;
		}

		int baseConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->ConfigProc(fa, ff, hwnd) : 1;
		}

		void baseStringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				f->StringProc(fa, ff, str);
			}
		}

		bool baseFssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen)
		{
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			return f ? f->FssProc(fa, ff, buf, buflen) : false;
		}

		void vobsubScriptConfig(IVDXScriptInterpreter* isi, void* lpVoid, VDXScriptValue* argv, int argc)
		{
			VDXFilterActivation* fa = (VDXFilterActivation*)lpVoid;
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f;
			}
			f = DNew CVobSubVirtualDubFilter(CString(*argv[0].asString()));
			*(CVirtualDubFilter**)fa->filter_data = f;
		}

		void textsubScriptConfig(IVDXScriptInterpreter* isi, void* lpVoid, VDXScriptValue* argv, int argc)
		{
			VDXFilterActivation* fa = (VDXFilterActivation*)lpVoid;
			CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
			if (f) {
				delete f;
			}
			f = DNew CTextSubVirtualDubFilter(CString(*argv[0].asString()), argv[1].asInt());
			*(CVirtualDubFilter**)fa->filter_data = f;
		}

		VDXScriptFunctionDef vobsub_func_defs[]= {
			{ (VDXScriptFunctionPtr)vobsubScriptConfig, "Config", "0s" },
			{ nullptr },
		};

		VDXScriptObject vobsub_obj= {
			nullptr, vobsub_func_defs
		};

		struct VDXFilterDefinition filterDef_vobsub = {
			nullptr, nullptr, nullptr,	// next, prev, module
			"VobSub",					// name
			"Adds subtitles from a vob sequence.", // desc
			"Gabest",					// maker
			nullptr,					// private_data
			sizeof(CVirtualDubFilter**), // inst_data_size
			vobsubInitProc,				// initProc
			baseDeinitProc,				// deinitProc
			baseRunProc,				// runProc
			baseParamProc,				// paramProc
			baseConfigProc,				// configProc
			baseStringProc,				// stringProc
			nullptr,					// startProc
			nullptr,					// endProc
			&vobsub_obj,				// script_obj
			baseFssProc,				// fssProc
		};

		VDXScriptFunctionDef textsub_func_defs[]= {
			{ (VDXScriptFunctionPtr)textsubScriptConfig, "Config", "0si" },
			{ nullptr },
		};

		VDXScriptObject textsub_obj= {
			nullptr, textsub_func_defs
		};

		struct VDXFilterDefinition filterDef_textsub = {
			nullptr, nullptr, nullptr,	// next, prev, module
			"TextSub",					// name
			"Adds subtitles from srt, sub, psb, smi, ssa, ass file formats.", // desc
			"Gabest",					// maker
			nullptr,					// private_data
			sizeof(CVirtualDubFilter**), // inst_data_size
			textsubInitProc,			// initProc
			baseDeinitProc,				// deinitProc
			baseRunProc,				// runProc
			baseParamProc,				// paramProc
			baseConfigProc,				// configProc
			baseStringProc,				// stringProc
			nullptr,					// startProc
			nullptr,					// endProc
			&textsub_obj,				// script_obj
			baseFssProc,				// fssProc
		};

		static VDXFilterDefinition* fd_vobsub;
		static VDXFilterDefinition* fd_textsub;

		extern "C" __declspec(dllexport) int __cdecl VirtualdubFilterModuleInit2(VDXFilterModule *fm, const VDXFilterFunctions *ff, int& vdfd_ver, int& vdfd_compat)
		{
			if (!(fd_vobsub = ff->addFilter(fm, &filterDef_vobsub, sizeof(VDXFilterDefinition)))
					|| !(fd_textsub = ff->addFilter(fm, &filterDef_textsub, sizeof(VDXFilterDefinition)))) {
				return 1;
			}

			vdfd_ver = VIRTUALDUB_FILTERDEF_VERSION;
			vdfd_compat = VIRTUALDUB_FILTERDEF_COMPATIBLE;

			return 0;
		}

		extern "C" __declspec(dllexport) void __cdecl VirtualdubFilterModuleDeinit(VDXFilterModule *fm, const VDXFilterFunctions *ff)
		{
			ff->removeFilter(fd_textsub);
			ff->removeFilter(fd_vobsub);
		}
	}
#endif
	//
	// Avisynth interface
	//

	namespace AviSynth1
	{
#include <avisynth/avisynth1.h>

		class CAvisynthFilter : public GenericVideoFilter, virtual public CFilter
		{
		public:
			CAvisynthFilter(PClip c, IScriptEnvironment* env) : GenericVideoFilter(c) {}

			PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) {
				PVideoFrame frame = child->GetFrame(n, env);

				env->MakeWritable(&frame);

				SubPicDesc dst;
				dst.w = vi.width;
				dst.h = vi.height;
				dst.pitch = frame->GetPitch();
				dst.bits = (void**)frame->GetWritePtr();
				dst.bpp = vi.BitsPerPixel();
				dst.type =
					vi.IsRGB32() ? ( env->GetVar("RGBA").AsBool() ? MSP_RGBA :MSP_RGB32) :
						vi.IsRGB24() ? MSP_RGB24 :
						vi.IsYUY2() ? MSP_YUY2 :
						-1;

				float fps = m_fps > 0 ? m_fps : (float)vi.fps_numerator / vi.fps_denominator;

				Render(dst, (REFERENCE_TIME)(10000000i64 * n / fps), fps);

				return(frame);
			}
		};

	class CVobSubAvisynthFilter : public CVobSubFilter, public CAvisynthFilter
		{
		public:
			CVobSubAvisynthFilter(PClip c, const char* fn, IScriptEnvironment* env)
				: CVobSubFilter(CString(fn))
				, CAvisynthFilter(c, env) {
				if (!m_pSubPicProvider) {
					env->ThrowError("VobSub: Can't open \"%s\"", fn);
				}
			}
		};

		AVSValue __cdecl VobSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			return(DNew CVobSubAvisynthFilter(args[0].AsClip(), args[1].AsString(), env));
		}

		class CTextSubAvisynthFilter : public CTextSubFilter, public CAvisynthFilter
		{
		public:
			CTextSubAvisynthFilter(PClip c, IScriptEnvironment* env, const char* fn, int CharSet = DEFAULT_CHARSET, float fps = -1)
				: CTextSubFilter(CString(fn), CharSet, fps)
				, CAvisynthFilter(c, env) {
				if (!m_pSubPicProvider)
					env->ThrowError("TextSub: Can't open \"%s\"", fn);
			}
		};

		AVSValue __cdecl TextSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			return(DNew CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString()));
		}

		AVSValue __cdecl TextSubCreateSI(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			return(DNew CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString(), args[2].AsInt()));
		}

		AVSValue __cdecl TextSubCreateSIF(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			return(DNew CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString(), args[2].AsInt(), args[3].AsFloat()));
		}

		AVSValue __cdecl MaskSubCreateSIIFI(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			AVSValue rgb32("RGB32");
			AVSValue  tab[5] = {
				args[1],
				args[2],
				args[3],
				args[4],
				rgb32
			};
			AVSValue value(tab,5);
			const char * nom[5]= {
				"width",
				"height",
				"fps",
				"length",
				"pixel_type"
			};
			AVSValue clip(env->Invoke("Blackness",value,nom));
			env->SetVar(env->SaveString("RGBA"),true);
			return(DNew CTextSubAvisynthFilter(clip.AsClip(), env, args[0].AsString()));
		}

		extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit(IScriptEnvironment* env)
		{
			env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
			env->AddFunction("TextSub", "cs", TextSubCreateS, 0);
			env->AddFunction("TextSub", "csi", TextSubCreateSI, 0);
			env->AddFunction("TextSub", "csif", TextSubCreateSIF, 0);
			env->AddFunction("MaskSub", "siifi", MaskSubCreateSIIFI, 0);
			env->SetVar(env->SaveString("RGBA"),false);
			return(nullptr);
		}
	}

	namespace AviSynth25
	{
#include <avisynth/avisynth25.h>

		static bool s_fSwapUV = false;

		class CAvisynthFilter : public GenericVideoFilter, virtual public CFilter
		{
		public:
			VFRTranslator *vfr;

			CAvisynthFilter(PClip c, IScriptEnvironment* env, VFRTranslator *_vfr=0) : GenericVideoFilter(c), vfr(_vfr) {}

			PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) {
				PVideoFrame frame = child->GetFrame(n, env);

				env->MakeWritable(&frame);

				SubPicDesc dst;
				dst.w = vi.width;
				dst.h = vi.height;
				dst.pitch = frame->GetPitch();
				dst.pitchUV = frame->GetPitch(PLANAR_U);
				dst.bits = (void**)frame->GetWritePtr();
				dst.bitsU = frame->GetWritePtr(PLANAR_U);
				dst.bitsV = frame->GetWritePtr(PLANAR_V);
				dst.bpp = dst.pitch/dst.w*8; //vi.BitsPerPixel();
				dst.type =
					vi.IsRGB32() ?( env->GetVar("RGBA").AsBool() ? MSP_RGBA : MSP_RGB32)  :
						vi.IsRGB24() ? MSP_RGB24 :
						vi.IsYUY2() ? MSP_YUY2 :
				/*vi.IsYV12()*/ vi.pixel_type == VideoInfo::CS_YV12 ? (s_fSwapUV?MSP_IYUV:MSP_YV12) :
				/*vi.IsIYUV()*/ vi.pixel_type == VideoInfo::CS_IYUV ? (s_fSwapUV?MSP_YV12:MSP_IYUV) :
						-1;

				float fps = m_fps > 0 ? m_fps : (float)vi.fps_numerator / vi.fps_denominator;

				REFERENCE_TIME timestamp;

				if (!vfr) {
					timestamp = (REFERENCE_TIME)(10000000i64 * n / fps);
				} else {
					timestamp = (REFERENCE_TIME)(10000000 * vfr->TimeStampFromFrameNumber(n));
				}

				Render(dst, timestamp, fps);

				return(frame);
			}
		};

		class CVobSubAvisynthFilter : public CVobSubFilter, public CAvisynthFilter
		{
		public:
			CVobSubAvisynthFilter(PClip c, const char* fn, IScriptEnvironment* env)
				: CVobSubFilter(CString(fn))
				, CAvisynthFilter(c, env) {
				if (!m_pSubPicProvider) {
					env->ThrowError("VobSub: Can't open \"%s\"", fn);
				}
			}
		};

		AVSValue __cdecl VobSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			return(DNew CVobSubAvisynthFilter(args[0].AsClip(), args[1].AsString(), env));
		}

		class CTextSubAvisynthFilter : public CTextSubFilter, public CAvisynthFilter
		{
		public:
			CTextSubAvisynthFilter(PClip c, IScriptEnvironment* env, const char* fn, int CharSet = DEFAULT_CHARSET, float fps = -1, VFRTranslator *vfr = 0) //vfr patch
				: CTextSubFilter(CString(fn), CharSet, fps)
				, CAvisynthFilter(c, env, vfr) {
				if (!m_pSubPicProvider)
					env->ThrowError("TextSub: Can't open \"%s\"", fn);
			}
		};

		AVSValue __cdecl TextSubCreateGeneral(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			if (!args[1].Defined())
				env->ThrowError("TextSub: You must specify a subtitle file to use");
			VFRTranslator *vfr = 0;
			if (args[4].Defined()) {
				vfr = GetVFRTranslator(args[4].AsString());
			}

			return(DNew CTextSubAvisynthFilter(
					   args[0].AsClip(),
					   env,
					   args[1].AsString(),
					   args[2].AsInt(DEFAULT_CHARSET),
					   args[3].AsFloat(-1),
					   vfr));
		}

		AVSValue __cdecl TextSubSwapUV(AVSValue args, void* user_data, IScriptEnvironment* env)
		{
			s_fSwapUV = args[0].AsBool(false);
			return(AVSValue());
		}

		AVSValue __cdecl MaskSubCreate(AVSValue args, void* user_data, IScriptEnvironment* env)/*SIIFI*/
		{
			if (!args[0].Defined())
				env->ThrowError("MaskSub: You must specify a subtitle file to use");
			if (!args[3].Defined() && !args[6].Defined())
				env->ThrowError("MaskSub: You must specify either FPS or a VFR timecodes file");
			VFRTranslator *vfr = 0;
			if (args[6].Defined()) {
				vfr = GetVFRTranslator(args[6].AsString());
			}

			AVSValue rgb32("RGB32");
			AVSValue fps(args[3].AsFloat(25));
			AVSValue  tab[6] = {
				args[1],
				args[2],
				args[3],
				args[4],
				rgb32
			};
			AVSValue value(tab,5);
			const char * nom[5]= {
				"width",
				"height",
				"fps",
				"length",
				"pixel_type"
			};
			AVSValue clip(env->Invoke("Blackness",value,nom));
			env->SetVar(env->SaveString("RGBA"),true);
			//return(new CTextSubAvisynthFilter(clip.AsClip(), env, args[0].AsString()));
			return(DNew CTextSubAvisynthFilter(
					   clip.AsClip(),
					   env,
					   args[0].AsString(),
					   args[5].AsInt(DEFAULT_CHARSET),
					   args[3].AsFloat(-1),
					   vfr));
		}

		extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env)
		{
#ifdef _VSMOD
			env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
			env->AddFunction("TextSubMod", "c[file]s[charset]i[fps]f[vfr]s", TextSubCreateGeneral, 0);
			env->AddFunction("TextSubSwapUVMod", "b", TextSubSwapUV, 0);
			env->AddFunction("MaskSubMod", "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s", MaskSubCreate, 0);
			env->SetVar(env->SaveString("RGBA"), false);
			return(nullptr);
#else
			env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
			env->AddFunction("TextSub", "c[file]s[charset]i[fps]f[vfr]s", TextSubCreateGeneral, 0);
			env->AddFunction("TextSubSwapUV", "b", TextSubSwapUV, 0);
			env->AddFunction("MaskSub", "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s", MaskSubCreate, 0);
			env->SetVar(env->SaveString("RGBA"),false);
			return(nullptr);
#endif
		}
	}

    //
    // VapourSynth interface
    //

    namespace VapourSynth {
#include <VapourSynth.h>
#include <VSHelper.h>

        class CTextSubVapourSynthFilter : public CTextSubFilter {
        public:
            CTextSubVapourSynthFilter(const wchar_t * file, const int charset, const float fps, int * error) : CTextSubFilter(CString(file), charset, fps) {
                *error = !m_pSubPicProvider ? 1 : 0;
            }
        };

        class CVobSubVapourSynthFilter : public CVobSubFilter {
        public:
            CVobSubVapourSynthFilter(const wchar_t * file, int * error) : CVobSubFilter(CString(file)) {
                *error = !m_pSubPicProvider ? 1 : 0;
            }
        };

        struct VSFilterData {
            VSNodeRef * node;
            const VSVideoInfo * vi;
            float fps;
            VFRTranslator * vfr;
            std::unique_ptr<CTextSubVapourSynthFilter> textsub;
            std::unique_ptr<CVobSubVapourSynthFilter> vobsub;
            VSFrameRef * buffer;
        };

        static inline __m128i _MM_PACKUS_EPI32(const __m128i & low, const __m128i & high) noexcept {
            const __m128i val_32 = _mm_set1_epi32(0x8000);
            const __m128i val_16 = _mm_set1_epi16(0x8000);
            const __m128i low1 = _mm_sub_epi32(low, val_32);
            const __m128i high1 = _mm_sub_epi32(high, val_32);
            return _mm_add_epi16(_mm_packs_epi32(low1, high1), val_16);
        }

        static void VS_CC vsfilterInit(VSMap * in, VSMap * out, void ** instanceData, VSNode * node, VSCore * core, const VSAPI * vsapi) {
            const VSFilterData * d = static_cast<const VSFilterData *>(*instanceData);
            vsapi->setVideoInfo(d->vi, 1, node);
        }

        static const VSFrameRef * VS_CC vsfilterGetFrame(int n, int activationReason, void ** instanceData, void ** frameData, VSFrameContext * frameCtx, VSCore * core, const VSAPI * vsapi) {
            const VSFilterData * d = static_cast<const VSFilterData *>(*instanceData);

            if (activationReason == arInitial) {
                vsapi->requestFrameFilter(n, d->node, frameCtx);
            } else if (activationReason == arAllFramesReady) {
                const VSFrameRef * src = vsapi->getFrameFilter(n, d->node, frameCtx);
                VSFrameRef * dst = vsapi->copyFrame(src, core);
                VSFrameRef * bgr = nullptr;

                SubPicDesc subpic;
                subpic.w = d->vi->width;
                subpic.h = d->vi->height;

                if (d->vi->format->id == pfYUV420P8) {
                    subpic.pitch = vsapi->getStride(dst, 0);
                    subpic.pitchUV = vsapi->getStride(dst, 1);
                    subpic.bits = vsapi->getWritePtr(dst, 0);
                    subpic.bitsU = vsapi->getWritePtr(dst, 1);
                    subpic.bitsV = vsapi->getWritePtr(dst, 2);
                    subpic.bpp = 8;
                    subpic.type = MSP_YV12;
                } else if (d->vi->format->id == pfYUV420P16) {
                    const int uvWidth = vsapi->getFrameWidth(src, 1);
                    const int uvWidthMod8 = uvWidth / 8 * 8;
                    const int uvStride = vsapi->getStride(src, 1) / sizeof(uint16_t);
                    const int bufStride = vsapi->getStride(d->buffer, 0);
                    const uint16_t * srcpY = reinterpret_cast<const uint16_t *>(vsapi->getReadPtr(src, 0));
                    const uint16_t * srcpU = reinterpret_cast<const uint16_t *>(vsapi->getReadPtr(src, 1));
                    const uint16_t * srcpV = reinterpret_cast<const uint16_t *>(vsapi->getReadPtr(src, 2));
                    uint8_t * VS_RESTRICT buffer = vsapi->getWritePtr(d->buffer, 0);

                    vs_bitblt(buffer, bufStride, srcpY, vsapi->getStride(src, 0), d->vi->width * sizeof(uint16_t), d->vi->height);
                    buffer += bufStride * d->vi->height;

                    for (int y = 0; y < vsapi->getFrameHeight(src, 1); y++) {
                        for (int x = 0; x < uvWidthMod8; x += 8) {
                            const __m128i u = _mm_load_si128(reinterpret_cast<const __m128i *>(srcpU + x));
                            const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i *>(srcpV + x));

                            const __m128i uvLow = _mm_unpacklo_epi16(u, v);
                            _mm_stream_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint32_t *>(buffer) + x), uvLow);

                            const __m128i uvHigh = _mm_unpackhi_epi16(u, v);
                            _mm_stream_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint32_t *>(buffer) + x + 4), uvHigh);
                        }

                        for (int x = uvWidthMod8; x < uvWidth; x++)
                            reinterpret_cast<uint32_t *>(buffer)[x] = (srcpV[x] << 16) | srcpU[x];

                        srcpU += uvStride;
                        srcpV += uvStride;
                        buffer += bufStride;
                    }

                    subpic.pitch = bufStride;
                    subpic.bits = vsapi->getWritePtr(d->buffer, 0);
                    subpic.bpp = 16;
                    subpic.type = MSP_P016;
                } else {
                    bgr = vsapi->newVideoFrame(vsapi->getFormatPreset(pfCompatBGR32, core), d->vi->width, d->vi->height, nullptr, core);

                    const int srcStride = vsapi->getStride(src, 0);
                    const int bgrStride = vsapi->getStride(bgr, 0);
                    const uint8_t * srcpR = vsapi->getReadPtr(src, 0);
                    const uint8_t * srcpG = vsapi->getReadPtr(src, 1);
                    const uint8_t * srcpB = vsapi->getReadPtr(src, 2);
                    uint8_t * VS_RESTRICT bgrp = vsapi->getWritePtr(bgr, 0);

                    bgrp += bgrStride * (d->vi->height - 1);

                    for (int y = 0; y < d->vi->height; y++) {
                        for (int x = 0; x < d->vi->width; x++) {
                            bgrp[x * 4 + 0] = srcpB[x];
                            bgrp[x * 4 + 1] = srcpG[x];
                            bgrp[x * 4 + 2] = srcpR[x];
                            bgrp[x * 4 + 3] = 0;
                        }

                        srcpR += srcStride;
                        srcpG += srcStride;
                        srcpB += srcStride;
                        bgrp -= bgrStride;
                    }

                    subpic.pitch = bgrStride;
                    subpic.bits = vsapi->getWritePtr(bgr, 0);
                    subpic.bpp = 32;
                    subpic.type = MSP_RGB32;
                }

                REFERENCE_TIME timestamp;
                if (!d->vfr)
                    timestamp = static_cast<REFERENCE_TIME>(10000000i64 * n / d->fps);
                else
                    timestamp = static_cast<REFERENCE_TIME>(10000000 * d->vfr->TimeStampFromFrameNumber(n));

                if (d->textsub)
                    d->textsub->Render(subpic, timestamp, d->fps);
                else
                    d->vobsub->Render(subpic, timestamp, d->fps);

                if (d->vi->format->id == pfYUV420P16) {
                    const int uvWidth = vsapi->getFrameWidth(dst, 1);
                    const int uvWidthMod8 = uvWidth / 8 * 8;
                    const int bufStride = vsapi->getStride(d->buffer, 0);
                    const int uvStride = vsapi->getStride(dst, 1) / sizeof(uint16_t);
                    const uint8_t * buffer = vsapi->getWritePtr(d->buffer, 0);
                    uint16_t * VS_RESTRICT dstpY = reinterpret_cast<uint16_t *>(vsapi->getWritePtr(dst, 0));
                    uint16_t * VS_RESTRICT dstpU = reinterpret_cast<uint16_t *>(vsapi->getWritePtr(dst, 1));
                    uint16_t * VS_RESTRICT dstpV = reinterpret_cast<uint16_t *>(vsapi->getWritePtr(dst, 2));

                    vs_bitblt(dstpY, vsapi->getStride(dst, 0), buffer, bufStride, d->vi->width * sizeof(uint16_t), d->vi->height);
                    buffer += bufStride * d->vi->height;

                    const __m128i mask = _mm_set1_epi32(0x0000FFFF);
                    for (int y = 0; y < vsapi->getFrameHeight(dst, 1); y++) {
                        for (int x = 0; x < uvWidthMod8; x += 8) {
                            const __m128i uvLow = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint32_t *>(buffer) + x));
                            const __m128i uvHigh = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint32_t *>(buffer) + x + 4));

                            const __m128i uLow = _mm_and_si128(uvLow, mask);
                            const __m128i uHigh = _mm_and_si128(uvHigh, mask);
                            const __m128i u = _MM_PACKUS_EPI32(uLow, uHigh);
                            _mm_stream_si128(reinterpret_cast<__m128i *>(dstpU + x), u);

                            const __m128i vLow = _mm_srli_epi32(uvLow, 16);
                            const __m128i vHigh = _mm_srli_epi32(uvHigh, 16);
                            const __m128i v = _MM_PACKUS_EPI32(vLow, vHigh);
                            _mm_stream_si128(reinterpret_cast<__m128i *>(dstpV + x), v);
                        }

                        for (int x = uvWidthMod8; x < uvWidth; x++) {
                            const uint32_t uv = reinterpret_cast<const uint32_t *>(buffer)[x];
                            dstpU[x] = uv & 0xFFFF;
                            dstpV[x] = uv >> 16;
                        }

                        buffer += bufStride;
                        dstpU += uvStride;
                        dstpV += uvStride;
                    }
                } else if (d->vi->format->id == pfRGB24) {
                    const int bgrStride = vsapi->getStride(bgr, 0);
                    const int dstStride = vsapi->getStride(dst, 0);
                    const uint8_t * bgrp = vsapi->getReadPtr(bgr, 0);
                    uint8_t * VS_RESTRICT dstpR = vsapi->getWritePtr(dst, 0);
                    uint8_t * VS_RESTRICT dstpG = vsapi->getWritePtr(dst, 1);
                    uint8_t * VS_RESTRICT dstpB = vsapi->getWritePtr(dst, 2);

                    bgrp += bgrStride * (d->vi->height - 1);

                    for (int y = 0; y < d->vi->height; y++) {
                        for (int x = 0; x < d->vi->width; x++) {
                            dstpB[x] = bgrp[x * 4 + 0];
                            dstpG[x] = bgrp[x * 4 + 1];
                            dstpR[x] = bgrp[x * 4 + 2];
                        }

                        bgrp -= bgrStride;
                        dstpR += dstStride;
                        dstpG += dstStride;
                        dstpB += dstStride;
                    }
                }

                vsapi->freeFrame(src);
                vsapi->freeFrame(bgr);
                return dst;
            }

            return nullptr;
        }

        static void VS_CC vsfilterFree(void * instanceData, VSCore * core, const VSAPI * vsapi) {
            VSFilterData * d = static_cast<VSFilterData *>(instanceData);
            vsapi->freeNode(d->node);
            vsapi->freeFrame(d->buffer);
            delete d;
        }

        static void VS_CC vsfilterCreate(const VSMap * in, VSMap * out, void * userData, VSCore * core, const VSAPI * vsapi) {
            std::unique_ptr<VSFilterData> d = std::make_unique<VSFilterData>();
            const std::string filterName{ static_cast<const char *>(userData) };
            int err{};

            d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
            d->vi = vsapi->getVideoInfo(d->node);

            try {
                if (!isConstantFormat(d->vi) ||
                    (d->vi->format->id != pfYUV420P8 && d->vi->format->id != pfYUV420P16 && d->vi->format->id != pfRGB24))
                    throw std::string{ "only constant format YUV420P8, YUV420P16, and RGB24 input supported" };

                const char * _file = vsapi->propGetData(in, "file", 0, nullptr);
                const int size = MultiByteToWideChar(CP_UTF8, 0, _file, -1, nullptr, 0);
                std::unique_ptr<wchar_t[]> file = std::make_unique<wchar_t[]>(size);
                MultiByteToWideChar(CP_UTF8, 0, _file, -1, file.get(), size);

                int charset = int64ToIntS(vsapi->propGetInt(in, "charset", 0, &err));
                if (err)
                    charset = DEFAULT_CHARSET;

                float fps = static_cast<float>(vsapi->propGetFloat(in, "fps", 0, &err));
                if (err)
                    fps = -1.0f;
                d->fps = (fps > 0.0f || !d->vi->fpsNum) ? fps : static_cast<float>(d->vi->fpsNum) / d->vi->fpsDen;

                const char * vfr = vsapi->propGetData(in, "vfr", 0, &err);
                if (!err)
                    d->vfr = GetVFRTranslator(vfr);

                if (!d->vi->fpsNum && fps <= 0.0f && !d->vfr)
                    throw std::string{ "variable framerate clip must have fps or vfr specified" };

#ifdef _VSMOD
				if (filterName == "TextSubMod")
#else
				if (filterName == "TextSub")
#endif
                    d->textsub = std::make_unique<CTextSubVapourSynthFilter>(file.get(), charset, fps, &err);
                else
                    d->vobsub = std::make_unique<CVobSubVapourSynthFilter>(file.get(), &err);
                if (err)
                    throw std::string{ "can't open " } + _file;

                if (d->vi->format->id == pfYUV420P16)
                    d->buffer = vsapi->newVideoFrame(vsapi->getFormatPreset(pfGray16, core), d->vi->width, d->vi->height + d->vi->height / 2, nullptr, core);
            } catch (const std::string & error) {
                vsapi->setError(out, (filterName + ": " + error).c_str());
                vsapi->freeNode(d->node);
                return;
            }

            vsapi->createFilter(in, out, static_cast<const char *>(userData), vsfilterInit, vsfilterGetFrame, vsfilterFree, fmParallelRequests, 0, d.release(), core);
        }

        //////////////////////////////////////////
        // Init

        VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin * plugin) {
#ifdef _VSMOD
			configFunc("com.holywu.vsfiltermod", "vsfmod", "VSFilterMod", VAPOURSYNTH_API_VERSION, 1, plugin);

			registerFunc("TextSubMod",
				"clip:clip;"
				"file:data;"
				"charset:int:opt;"
				"fps:float:opt;"
				"vfr:data:opt;",
				vsfilterCreate, const_cast<char*>("TextSubMod"), plugin);

			registerFunc("VobSub",
				"clip:clip;"
				"file:data;",
				vsfilterCreate, const_cast<char*>("VobSub"), plugin);
#else
            configFunc("com.holywu.vsfilter", "vsf", "VSFilter", VAPOURSYNTH_API_VERSION, 1, plugin);
            
            registerFunc("TextSub",
                         "clip:clip;"
                         "file:data;"
                         "charset:int:opt;"
                         "fps:float:opt;"
                         "vfr:data:opt;",
                         vsfilterCreate, const_cast<char *>("TextSub"), plugin);
            
            registerFunc("VobSub",
                         "clip:clip;"
                         "file:data;",
                         vsfilterCreate, const_cast<char *>("VobSub"), plugin);
#endif
        }
    }

}

UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uiMsg) {
		case WM_NOTIFY: {
			OPENFILENAME* ofn = ((OFNOTIFY *)lParam)->lpOFN;

			if (((NMHDR *)lParam)->code == CDN_FILEOK) {
				ofn->lCustData = (LPARAM)CharSetList[SendMessageW(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0)];
			}

			break;
		}

		case WM_INITDIALOG: {
			SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);

			for (ptrdiff_t i = 0; i < CharSetLen; i++) {
				CString s;
				s.Format(L"%s (%d)", CharSetNames[i], CharSetList[i]);
				SendMessageW(GetDlgItem(hDlg, IDC_COMBO1), CB_ADDSTRING, 0, (LONG)(LPCTSTR)s);
				if (CharSetList[i] == (int)((OPENFILENAME*)lParam)->lCustData) {
					SendMessageW(GetDlgItem(hDlg, IDC_COMBO1), CB_SETCURSEL, i, 0);
				}
			}

			break;
		}

		default:
			break;
	}

	return FALSE;
}
