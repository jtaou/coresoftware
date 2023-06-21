// Tell emacs that this is a C++ source
//  -*- C++ -*-.
#ifndef FUN4ALLRAW_FUN4ALLPRDFINPUTCOMBINERMANAGER_H
#define FUN4ALLRAW_FUN4ALLPRDFINPUTCOMBINERMANAGER_H

#include <fun4all/Fun4AllInputManager.h>

#include <Event/phenixTypes.h>

#include <map>
#include <string>

class Event;
class SinglePrdfInput;
class oEvent;
class Packet;
class PHCompositeNode;
class SyncObject;

class Fun4AllPrdfInputCombinerManager : public Fun4AllInputManager
{
 public:
  Fun4AllPrdfInputCombinerManager(const std::string &name = "DUMMY", const std::string &prdfnodename = "PRDF", const std::string &topnodename = "TOP");
  ~Fun4AllPrdfInputCombinerManager() override;
  int fileopen(const std::string &filenam) override {return 0;}
//cppcheck-suppress virtualCallInConstructor
  int fileclose() override;
  int run(const int nevents = 0) override;

  void Print(const std::string &what = "ALL") const override;
  int ResetEvent() override;
  int PushBackEvents(const int i) override;
  int GetSyncObject(SyncObject **mastersync) override;
  int SyncIt(const SyncObject *mastersync) override;
  int HasSyncObject() const  override {return 1;}
  std::string GetString(const std::string &what) const override;
//  Eventiterator *AddPrdfInputList(const std::string &listfile);
  SinglePrdfInput *AddPrdfInputFile(const std::string &filename);
  void AddPacket(const int evtno, Packet *p);
  void UpdateEventFoundCounter(const int evtno);

 private:
struct PacketInfo
{
  std::vector<Packet*> PacketVector;
  unsigned int EventFoundCounter = 0;
};

  int m_Segment = -999;
  int m_EventsTotal = 0;
  int m_EventsThisFile = 0;
  int m_RunNumber = 0;
  unsigned int m_CombinerDepth = 10;
  unsigned int m_LowWaterMark = 5;
  std::vector<SinglePrdfInput *> m_PrdfInputVector;
  SyncObject *m_SyncObject = nullptr;
  PHCompositeNode *m_topNode = nullptr;
  Event *m_Event = nullptr;
  Event *m_SaveEvent = nullptr;
  PHDWORD workmem[4*1024*1024];
  oEvent *oph;

  std::map<int, Event *> m_EventCombiner;
  std::map<int, std::vector<Packet*>> m_EventMap;
  std::map<int, PacketInfo> m_PacketMap;
  std::string m_PrdfNodeName;
};

#endif /* FUN4ALL_FUN4ALLPRDFINPUTCOMBINERMANAGER_H */
