// SPDX-License-Identifier: LGPL-2.1-only
// Native path storage and SDK-defined COM transport. Rendering/tessellation
// remain explicit unsupported consumers; storing a path never fabricates pixels.
#include "d2d1_internal.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <cstdio>
#include <cstring>
#include <set>
namespace vitreo::winapi {
using namespace d2d1_contract;
namespace {
u32 geometry_gap(const char* method){static std::mutex mutex;static std::set<std::string> reported;std::lock_guard lock(mutex);if(reported.insert(method).second)std::fprintf(stderr,"[vitreo] UNIMPLEMENTED d2d1.dll!%s: geometric operation unavailable\n",method);return E_NOTIMPL;}
}
GuestAddr D2DManager::allocate_geometry(GuestAddr factory,bool sink,std::shared_ptr<GeometryState> state){
 const auto base=m_memory.map_first_fit(0x60000000,GUEST_PAGE_SIZE,MemoryProtection::ReadWrite);if(!base)return 0;
 const usize width=m_is_64bit?8:4;const auto put=[&](GuestAddr at,GuestAddr value){return m_is_64bit?m_memory.write_u64(at,value).is_ok():m_memory.write_u32(at,static_cast<u32>(value)).is_ok();};
 bool ok=put(base,base+width);for(usize i=0;ok&&i<(sink?m_sink_methods.size():m_path_methods.size());++i)ok=put(base+width*(i+1),sink?m_sink_methods[i]:m_path_methods[i]);
 if(!ok){(void)m_memory.unmap_region(base,GUEST_PAGE_SIZE);return 0;}
 m_geometries.emplace(base,GeometryObject{1,factory,sink,std::move(state)});++m_factories.at(factory).references;return base;
}
void D2DManager::create_path_geometry(Marshaler& m,GuestAddr factory){
 const auto out=m.arg_ptr(1);if(!out){m.set_return(E_POINTER);return;}if(!write_pointer(m,out,0))return;
 const auto object=allocate_geometry(factory,false,std::make_shared<GeometryState>());if(!object){m.set_return(E_OUTOFMEMORY);return;}
 if(!write_pointer(m,out,object)){m_geometries.erase(object);release_factory(factory);(void)m_memory.unmap_region(object,GUEST_PAGE_SIZE);return;}m.set_return(S_OK);
}
void D2DManager::geometry_method(Marshaler& m,usize slot,bool sink){
 std::lock_guard lock(m_mutex);const auto self=m.arg_ptr(0);auto it=m_geometries.find(self);
 if(it==m_geometries.end()||it->second.sink!=sink){(void)m.raise_access_violation(false,self);return;}
 auto& object=it->second;auto state=object.state;
 if(slot==0){const auto out=m.arg_ptr(2);if(!out){m.set_return(E_POINTER);return;}if(!write_pointer(m,out,0))return;std::array<u8,16> iid{};if(!read_iid(m,m.arg_ptr(1),iid))return;
 const bool accepted=iid==unknown_iid||(sink?(iid==iid_ID2D1SimplifiedGeometrySink||iid==iid_ID2D1GeometrySink):(iid==iid_ID2D1Resource||iid==iid_ID2D1Geometry||iid==iid_ID2D1PathGeometry));
 if(!accepted){m.set_return(E_NOINTERFACE);return;}if(!write_pointer(m,out,self))return;++object.references;m.set_return(S_OK);return;}
 if(slot==1){m.set_return(++object.references);return;}
 if(slot==2){const auto remaining=--object.references;if(!remaining){const auto factory=object.factory;m_geometries.erase(it);(void)m_memory.unmap_region(self,GUEST_PAGE_SIZE);release_factory(factory);}m.set_return(remaining);return;}
 if(!sink){
  if(slot==path_GetFactory){if(write_pointer(m,m.arg_ptr(1),object.factory))++m_factories.at(object.factory).references;return;}
  if(slot==path_Open){const auto out=m.arg_ptr(1);if(!out){m.set_return(E_POINTER);return;}if(!write_pointer(m,out,0))return;if(state->opened){m.set_return(D2DERR_WRONG_STATE);return;}const auto factory=object.factory;const auto result=allocate_geometry(factory,true,state);if(!result){m.set_return(E_OUTOFMEMORY);return;}if(!write_pointer(m,out,result)){m_geometries.erase(result);release_factory(factory);(void)m_memory.unmap_region(result,GUEST_PAGE_SIZE);return;}state->opened=true;m.set_return(S_OK);return;}
  if(!state->closed){m.set_return(D2DERR_WRONG_STATE);return;}if(state->error!=S_OK){m.set_return(state->error);return;}
  if(slot==path_GetFigureCount||slot==path_GetSegmentCount){if(!m.arg_ptr(1)){m.set_return(E_POINTER);return;}
   // The SDK does not specify whether an implicit closing edge contributes
   // to this count. Until measured, never publish an invented count.
   if(slot==path_GetSegmentCount&&state->closed_edge){m.set_return(geometry_gap("ID2D1PathGeometry::GetSegmentCount implicit closing edge"));return;}
   const u32 value=slot==path_GetFigureCount?state->figures:state->segments;if(!write_data(m,m.arg_ptr(1),&value,sizeof(value)))return;m.set_return(S_OK);return;}
  if(slot==path_Stream){auto dest=m_geometries.find(m.arg_ptr(1));if(dest==m_geometries.end()||!dest->second.sink){m.set_return(geometry_gap("ID2D1PathGeometry::Stream external sink"));return;}auto& d=*dest->second.state;if(d.closed||d.figure){m.set_return(D2DERR_WRONG_STATE);return;}if(d.figures){m.set_return(geometry_gap("ID2D1PathGeometry::Stream nonempty sink"));return;}d.commands=state->commands;d.figures=state->figures;d.segments=state->segments;d.closed_edge=state->closed_edge;d.fill=state->fill;d.flags=state->flags;m.set_return(S_OK);return;}
  m.set_return(geometry_gap(path_methods[slot].name));return;
 }
 if(slot==sink_Close){if(state->closed){m.set_return(D2DERR_WRONG_STATE);return;}if(state->figure)state->latch(D2DERR_WRONG_STATE);state->closed=true;m.set_return(state->error);return;}
 if(state->closed)return;if(state->error!=S_OK)return;
 if(slot==sink_SetFillMode){if(state->figures){state->latch(D2DERR_WRONG_STATE);return;}const u32 fill=m.arg(1);if(fill!=D2D1_FILL_MODE_ALTERNATE&&fill!=D2D1_FILL_MODE_WINDING){state->latch(E_INVALIDARG);return;}state->fill=fill;return;}
 if(slot==sink_SetSegmentFlags){const u32 flags=m.arg(1);if(flags&~(D2D1_PATH_SEGMENT_FORCE_UNSTROKED|D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN)){state->latch(E_INVALIDARG);return;}state->flags=flags;state->commands.push_back({slot,flags,{}});return;}
 const auto point=[&](){const u64 value=m_is_64bit?m.arg(1):u64(static_cast<u32>(m.arg(1)))|(u64(static_cast<u32>(m.arg(2)))<<32);std::vector<u8> bytes(8);std::memcpy(bytes.data(),&value,8);return bytes;};
 if(slot==sink_BeginFigure){if(state->figure){state->latch(D2DERR_WRONG_STATE);return;}const u32 begin=m.arg(m_is_64bit?2:3);if(begin!=D2D1_FIGURE_BEGIN_FILLED&&begin!=D2D1_FIGURE_BEGIN_HOLLOW){state->latch(E_INVALIDARG);return;}state->commands.push_back({slot,begin,point()});state->figure=true;++state->figures;return;}
 if(!state->figure){state->latch(D2DERR_WRONG_STATE);return;}
 if(slot==sink_EndFigure){const u32 end=m.arg(1);if(end!=D2D1_FIGURE_END_OPEN&&end!=D2D1_FIGURE_END_CLOSED){state->latch(E_INVALIDARG);return;}state->commands.push_back({slot,end,{}});state->figure=false;state->closed_edge|=end==D2D1_FIGURE_END_CLOSED;return;}
 if(slot==sink_AddLine){state->commands.push_back({slot,state->flags,point()});++state->segments;return;}
 usize size=0;u32 count=1;
 if(slot==sink_AddLines){size=8;count=m.arg(2);}else if(slot==sink_AddBeziers){size=size_D2D1_BEZIER_SEGMENT;count=m.arg(2);}else if(slot==sink_AddQuadraticBeziers){size=size_D2D1_QUADRATIC_BEZIER_SEGMENT;count=m.arg(2);}else if(slot==sink_AddBezier)size=size_D2D1_BEZIER_SEGMENT;else if(slot==sink_AddQuadraticBezier)size=size_D2D1_QUADRATIC_BEZIER_SEGMENT;else if(slot==sink_AddArc)size=size_D2D1_ARC_SEGMENT;
 if(!size){state->latch(geometry_gap(sink_methods[slot].name));return;}
 for(u32 i=0;i<count;++i){std::vector<u8> bytes(size);if(!read_data(m,m.arg_ptr(1)+usize(i)*size,bytes.data(),size)){state->latch(E_POINTER);return;}state->commands.push_back({slot,state->flags,std::move(bytes)});++state->segments;}
}
} // namespace vitreo::winapi
