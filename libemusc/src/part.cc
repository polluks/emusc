/*  
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */


#include "part.h"

#include <algorithm>
#include <cmath>
#include <iostream>


namespace EmuSC {

Part::Part(uint8_t id, uint8_t mode, uint8_t type,
	   ControlRom &ctrlRom, PcmRom &pcmRom)
  : _id(id),
    _midiChannel(id),
    _instrument(0),
    _drumSet(0),
    _volume(100),
    _pan(64),
    _reverb(40),
    _chorus(0),
    _keyShift(0),
    _mode(mode_Norm),
    _bendRange(2),
    _modDepth(10),
    _keyRangeL(24),                     // => C1
    _keyRangeH(127),                    // => G9
    _velSensDepth(64),
    _velSensOffset(64),
    _partialReserve(2),
    _polyMode(true),
    _vibRate(0),
    _vibDepth(0),
    _vibDelay(0),
    _cutoffFreq(0),
    _resonance(0),
    _attackTime(0),
    _decayTime(0),
    _releaseTime(0),
    _pitchBend(0),
    _mute(false),
    _modulation(0),
    _expression(127),
    _portamento(false),
    _holdPedal(false),
    _7bScale(1/127.0),
    _lastSample(0),
    _ctrlRom(ctrlRom),
    _pcmRom(pcmRom)
{
  // Part 10 is factory preset for MIDI channel 10 and standard drum set
  if (id == 9)
    _mode = 1;

  // Drum sets as defined inn owner's manual
  // TODO: Take into account which mode is set in addition to actual ROM files
  if (ctrlRom.synthModel == ControlRom::sm_SC55 ||
      ctrlRom.synthModel == ControlRom::sm_SC55mkII)
    _drumSetBanks = ctrlRom.get_drum_set_banks(ControlRom::sm_SC55);
  else if (ctrlRom.synthModel == ControlRom::sm_SC88)
    _drumSetBanks = ctrlRom.get_drum_set_banks(ControlRom::sm_SC88);
  else if (ctrlRom.synthModel == ControlRom::sm_SC88Pro)
    _drumSetBanks = ctrlRom.get_drum_set_banks(ControlRom::sm_SC88Pro);
}


Part::~Part()
{
  clear_all_notes();
}


// Parts always produce 2 channel & 32kHz (native) output. Other channel
// numbers and sample rates are handled by the calling Synth class.
int Part::get_next_sample(float *sampleOut)
{
  // Return immediately if we have no notes to play
  if (_notes.size() == 0)
    return 0;

  float partSample[2] = { 0, 0 };
  float accSample = 0;

  // Get next sample from active notes, delete those which are finished
  std::list<Note*>::iterator itr = _notes.begin();
  while (itr != _notes.end()) {
    bool finished = (*itr)->get_next_sample(partSample, _pitchBend);

    if (finished) {
//      std::cout << "Both partials have finished -> delete note" << std::endl;
      delete *itr;
      itr = _notes.erase(itr);
    } else {
      ++itr;
    }
  }

  // Apply volume from part (MIDI channel) and expression (CM11)
  partSample[0] *= _volume * _7bScale * (_expression * _7bScale);
  partSample[1] *= _volume * _7bScale * (_expression * _7bScale);

  // Store last (highest) value for future queries (typically for bar display)
  _lastSample = (_lastSample >= partSample[0]) ? _lastSample : partSample[0];
  
  // Apply pan from part (MIDI Channel)
  if (_pan > 64)
    partSample[0] *= 1.0 - (_pan - 64) / 63.0;
  else if (_pan < 64)
    partSample[1] *= ((_pan - 1) / 64.0);

  sampleOut[0] += partSample[0];
  sampleOut[1] += partSample[1];

  return 0;
}


float Part::get_last_sample(void)
{
  float ret = _lastSample;
  _lastSample = 0;

  return ret;
}


int Part::get_num_partials(void)
{
  if (_notes.size() == 0)
    return 0;

  int numPartials = 0;
  for (auto &n: _notes)
    numPartials += n->get_num_partials();

  return numPartials;
}


// Should mute => not accept key - or play silently in the background?
int Part::add_note(uint8_t midiChannel, uint8_t key, uint8_t velocity,
		   uint32_t sampleRate)
{
  // 1. Check if this message is relevant for this part
  if (midiChannel != _midiChannel || _mute)
    return 0;

  // 2. Find partial(s) used by instrument or drum set
  uint16_t instrumentIndex = (_mode == mode_Norm) ? _instrument :
                             _ctrlRom.drumSet(_drumSet).preset[key];
  if (instrumentIndex == 0xffff)        // Ignore undefined instruments / drums
    return 0;

  int drumSet = (_mode == mode_Norm) ? -1 : _drumSet;

  // 3. If note is a drum -> check if drum accepts note on
  if (drumSet >= 0 && !(_ctrlRom.drumSet(drumSet).flags[key] & 0x10))
    return 0;

  // 4. Create new note and set default values (note: using pointers)
  Note *n = new Note(key, velocity, instrumentIndex, drumSet,
		     _ctrlRom, _pcmRom, sampleRate);
  _notes.push_back(n);

  if (0)
    std::cout << "EmuSC: New note [ part=" << (int) _id
	      << " key=" << (int) key
	      << " velocity=" << (int) velocity
	      << " preset=" << _ctrlRom.instrument(instrumentIndex).name
	      << " ]" << std::endl;
  
    return 1;
}


int Part::stop_note(uint8_t midiChannel, uint8_t key)
{
  // 1. Check if this message is relevant for this part. Check:Hanging note bug?
  if (midiChannel != _midiChannel)
    return 0;

  // 2. Check if CM64 is active (Hold Pedal) and store notes if true
  if (_holdPedal) {
    _holdPedalKeys.push_back(key);
    return 0;
  }

  // 3. Else iterate through notes list and send stop signal (-> release)
  int i;
  for (auto &n : _notes) {
    bool ret = n->stop(key);
    i += ret;
  }

  return i;
}


int Part::clear_all_notes(void)
{
  int i = _notes.size();
  for (auto n : _notes)
    delete n;

  _notes.clear();

  return i;
}


int Part::set_program(uint8_t midiChannel, uint8_t index, uint8_t bank)
{
  if (midiChannel != _midiChannel)
    return 0;

  // Finds correct instrument variation from variations table
  // Implemented according to SC-55 Owner's Manual page 42-45
  _instrument = _ctrlRom.variation(bank).variation[index];
  if (bank < 63 && index < 120)
    while (_instrument == 0xffff)
      _instrument = _ctrlRom.variation(--bank).variation[index];

  // If part is used for drums, select correct drum set
  if (_mode != mode_Norm) {
    std::list<int>::iterator it = std::find(_drumSetBanks.begin(),
					    _drumSetBanks.end(),
					    (int) index);
    if (it != _drumSetBanks.end())
      _drumSet = (int8_t) std::distance(_drumSetBanks.begin(), it);
    else
      std::cerr << "EmuSC: Illegal program for drum set (" << (int) index << ")"
		<< std::endl;
  }

  return 1;
}


int Part::set_control(enum ControlMsg m, uint8_t midiChannel, uint8_t value)
{
  if (midiChannel != _midiChannel)
    return 0;

  if (m == cmsg_Volume) {
    _volume = value;
  }  else if (m == cmsg_ModWheel) {
    _modulation = value;
  }  else if (m == cmsg_Pan) {
    _pan = (value == 0) ? 1 : value;
  }  else if (m == cmsg_Expression) {
    _expression =  value;
  }  else if (m == cmsg_HoldPedal) {
    _holdPedal =  (value >= 64) ? true : false;
    if (_holdPedal == false) {
      for (auto &k : _holdPedalKeys)
	stop_note(midiChannel, k);
      _holdPedalKeys.clear();
    }
  }  else if (m == cmsg_Portamento) {
    _portamento =  (value >= 64) ? true : false;
  }  else if (m == cmsg_PortamentoTime) {
    _portamentoTime =  value;
  }  else if (m == cmsg_Reverb) {
    _reverb =  value;
  }  else if (m == cmsg_Chorus) {
    _chorus =  value;
  }
  
  return 1;
}


// Note: One semitone = log(2)/12
int Part::set_pitchBend(uint8_t midiChannel, int16_t pitchBend)
{
  if (midiChannel != _midiChannel)
    return 0;

  _pitchBend = exp(((pitchBend-8192)/8192.0) * _bendRange * 0.5  * (log(2)/12)) - 1;

  return 1;
}

}
