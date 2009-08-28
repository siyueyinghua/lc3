/*\
 *  LC-3 Simulator
 *  Copyright (C) 2004  Anthony Liguori <aliguori@cs.utexas.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\*/

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include "cpu.hpp"
#include "memory.hpp"
#include "hardware.hpp"

struct KBSR : public MappedWord
{
  enum { ADDRESS = 0xFE00 };
  operator int16_t() const { return 0x8000; }
};

struct KBDR : public MappedWord
{
  enum { ADDRESS = 0xFE02 };

  operator int16_t() const {
    struct termios new_term, old_term;
    tcgetattr(fileno(stdin), &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(fileno(stdin), TCSAFLUSH, &new_term);
    int ret = getchar();
    tcsetattr(fileno(stdin), TCSAFLUSH, &old_term);
    return ret;
  }
};

struct DSR : public MappedWord
{
  enum { ADDRESS = 0xFE04 };
  operator int16_t() const { return 0x8000; }
};

struct DDR : public MappedWord
{
  enum { ADDRESS = 0xFE06 };
  DDR() : fd(-1) { }
  DDR &operator=(int16_t data) {
    if (fd == -1) {
      putchar(data & 0xFF);
      fflush(stdout);
    } else {
      int8_t byte = data & 0xFF;
      write(fd, &byte, 1);
    }
    return *this;
  }

  void set_tty(int _fd) {
    fd = _fd;
  }

private:
  int fd;
};

struct CCR : public MappedWord
{
  enum { ADDRESS = 0xFFFF };
  operator int16_t() const { return (int16_t)ccr; }
  operator uint16_t() const { return ccr; }
  CCR &operator=(int16_t value) {
    ccr = (uint16_t)value;
    return *this;
  }
  void mycycle() {
    ccr++;
  }
private:
  uint16_t ccr;
};

struct MCR : public MappedWord
{
  MCR(CCR &ccr, LC3::CPU &cpu) : mcr(0x0030), ccr(ccr), cpu(cpu) { }

  enum { ADDRESS = 0xFFFE };
  operator int16_t() const { return mcr; }
  MCR &operator=(int16_t value) {
    mcr = value;
    return *this;
  }

  void cycle() {
    ccr.mycycle();
    if ((uint16_t)ccr >= (mcr & 0x3FFF) && 
	mcr & 0x4000) {
      cpu.interrupt(0x02, 1);
    }
  }
private:
  int16_t mcr;
  CCR &ccr;
  LC3::CPU &cpu;
};

class Hardware::Implementation
{
public:
  Implementation(Memory &mem, LC3::CPU &cpu) : 
    mem(mem), mcr(ccr, cpu)
  {
    mem.register_dma(KBSR::ADDRESS, &kbsr);
    mem.register_dma(KBDR::ADDRESS, &kbdr);
    mem.register_dma(DSR::ADDRESS, &dsr);
    mem.register_dma(DDR::ADDRESS, &ddr);
    mem.register_dma(MCR::ADDRESS, &mcr);
    mem.register_dma(CCR::ADDRESS, &ccr);
  }

  void set_tty(int fd) {
    ddr.set_tty(fd);
  }

private:
  Memory &mem;
  KBSR kbsr;
  KBDR kbdr;
  DSR dsr;
  DDR ddr;
  CCR ccr;
  MCR mcr;
};

Hardware::Hardware(Memory &mem, LC3::CPU &cpu) :
  impl(new Implementation(mem, cpu))
{
}

Hardware::~Hardware()
{
  delete impl;
}

void Hardware::set_tty(int fd)
{
  impl->set_tty(fd);
}