/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Andrew Bardsley
 */

#include <iomanip>
#include <sstream>

#include "arch/isa.hh"
#include "arch/registers.hh"
#include "cpu/minor/dyn_inst.hh"
#include "cpu/minor/trace.hh"
#include "cpu/base.hh"
#include "cpu/reg_class.hh"
#include "debug/MinorExecute.hh"
#include "enums/OpClass.hh"
#include "debug/RegFileAccess.hh"
#include "debug/FUsREG.hh"
#include "debug/BranchsREG.hh"
#include "debug/CMPsREG.hh"
namespace Minor
{

	std::ostream &
		operator <<(std::ostream &os, const InstId &id)
		{
			os << id.threadId << '/' << id.streamSeqNum << '.'
				<< id.predictionSeqNum << '/' << id.lineSeqNum;

			/* Not all structures have fetch and exec sequence numbers */
			if (id.fetchSeqNum != 0) {
				os << '/' << id.fetchSeqNum;
				if (id.execSeqNum != 0)
					os << '.' << id.execSeqNum;
			}

			return os;
		}

	MinorDynInstPtr MinorDynInst::bubbleInst = NULL;

	void
		MinorDynInst::init()
		{
			if (!bubbleInst) {
				bubbleInst = new MinorDynInst();
				assert(bubbleInst->isBubble());
				/* Make bubbleInst immortal */
				bubbleInst->incref();
			}
		}

	bool
		MinorDynInst::isLastOpInInst() const
		{
			assert(staticInst);
			return !(staticInst->isMicroop() && !staticInst->isLastMicroop());
		}

	bool
		MinorDynInst::isNoCostInst() const
		{
			return isInst() && staticInst->opClass() == No_OpClass;
		}

	void
		MinorDynInst::reportData(std::ostream &os) const
		{
			if (isBubble())
				os << "-";
			else if (isFault())
				os << "F;" << id;
			else
				os << id;
		}

	std::ostream &
		operator <<(std::ostream &os, const MinorDynInst &inst)
		{
			os << inst.id << " pc: 0x"
				<< std::hex << inst.pc.instAddr() << std::dec << " (";

			if (inst.isFault())
				os << "fault: \"" << inst.fault->name() << '"';
			else if (inst.staticInst)
				os << inst.staticInst->getName();
			else
				os << "bubble";

			os << ')';

			return os;
		}

	/** Print a register in the form x<n>, f<n>, <n>(<name>), z for integer,
	 *  float, misc and zero registers given an 'architectural register number' */
	static void
		printRegName(std::ostream &os, TheISA::RegIndex reg)
		{
			RegClass reg_class = regIdxToClass(reg);

			switch (reg_class)
			{
				case MiscRegClass:
					{
						TheISA::RegIndex misc_reg = reg - TheISA::Misc_Reg_Base;

						/* This is an ugly test because not all archs. have miscRegName */
#if THE_ISA == ARM_ISA
						os << 'm' << misc_reg << '(' << TheISA::miscRegName[misc_reg] <<
							')';
#else
						os << 'n' << misc_reg;
#endif
					}
					break;
				case FloatRegClass:
					os << 'f' << static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);
					break;
				case IntRegClass:
					if (reg == TheISA::ZeroReg) {
						os << 'z';
					} else {
						os << 'r' << static_cast<unsigned int>(reg);
					}
					break;
				case CCRegClass:
					os << 'c' << static_cast<unsigned int>(reg - TheISA::CC_Reg_Base);
			}
		}
	static void
		printRegNameminorRegAccess(std::ostringstream &regs_str, TheISA::RegIndex reg, bool isSource, const MinorDynInst* inst) //const MinorDynInstPtr inst)
		{
			std::string funcName="nothing";
			Addr sym_addr;
			debugSymbolTable->findNearestSymbol(inst->pc.instAddr() , funcName, sym_addr);
			std::ostringstream os;
			//os <<"  " << inst->staticInst->disassemble(0)<<":";

			RegClass reg_class = regIdxToClass(reg);
			std::string preFix;

			if(isSource) preFix="Src";
			else  preFix="Des";

			switch (reg_class)
			{
				case MiscRegClass:
					{
						break;
						TheISA::RegIndex misc_reg = reg - TheISA::Misc_Reg_Base;

						/* This is an ugly test because not all archs. have miscRegName */
#if THE_ISA == ARM_ISA
						os << preFix << "M:" << misc_reg << '(' << TheISA::miscRegName[misc_reg] <<
							')';
						DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(RegFileAccess,  "		 	: %s\n", os.str());
#else
						os << preFix << "N:" << misc_reg;
						DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(RegFileAccess,  "		 	: %s\n", os.str());
#endif
					}
				case FloatRegClass:
					//regID=static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);

					os << preFix << "F:" << static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);
					DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
					DPRINTF(RegFileAccess,  "		 	: %s\n", os.str() );
					break;
				case IntRegClass:
					//regID=static_cast<unsigned int>(reg);
					if (reg == TheISA::ZeroReg) {

						os << preFix << "Z:" << static_cast<unsigned int>(reg);
						DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(RegFileAccess,  "		 	: %s\n", os.str());
					} else {

						os << preFix << "X:" << static_cast<unsigned int>(reg);
						DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(RegFileAccess,  "		 	: %s\n", os.str());
					}
					break;
				case CCRegClass:
					break;
					os << preFix << "C:" << static_cast<unsigned int>(reg - TheISA::CC_Reg_Base);
					DPRINTF(RegFileAccess,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
					DPRINTF(RegFileAccess,  "		 	: %s\n", os.str());

			}
		}
	static void
		printRegNameFUs(std::ostringstream &regs_str, TheISA::RegIndex reg, bool isSource, const MinorDynInst* inst) //const MinorDynInstPtr inst)
		{
			std::string funcName="nothing";
			Addr sym_addr;
			debugSymbolTable->findNearestSymbol(inst->pc.instAddr() , funcName, sym_addr);
			std::ostringstream os;
			//os <<"  " << inst->staticInst->disassemble(0)<<":";

			RegClass reg_class = regIdxToClass(reg);
			std::string preFix;

			if(isSource) preFix="Src";
			else  preFix="Des";

			switch (reg_class)
			{
				case MiscRegClass:
					{
						break;
						TheISA::RegIndex misc_reg = reg - TheISA::Misc_Reg_Base;

						/* This is an ugly test because not all archs. have miscRegName */
#if THE_ISA == ARM_ISA
						os << preFix << "M:" << misc_reg << '(' << TheISA::miscRegName[misc_reg] <<
							')';
						//DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						//DPRINTF(FUsREG,  "		 	: %s\n", os.str());
#else
						os << preFix << "N:" << misc_reg;
						//DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						//DPRINTF(FUsREG,  "		 	: %s\n", os.str());
#endif
					}
				case FloatRegClass:
					//regID=static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);

					os << preFix << "F:" << static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);
					DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
					DPRINTF(FUsREG,  "		 	: %s\n", os.str() );
					break;
				case IntRegClass:
					//regID=static_cast<unsigned int>(reg);
					if (reg == TheISA::ZeroReg) {

						os << preFix << "Z:" << static_cast<unsigned int>(reg);
						DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(FUsREG,  "		 	: %s\n", os.str());
					} else {

						os << preFix << "X:" << static_cast<unsigned int>(reg);
						DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
						DPRINTF(FUsREG,  "		 	: %s\n", os.str());
					}
					break;
				case CCRegClass:
					break;
					os << preFix << "C:" << static_cast<unsigned int>(reg - TheISA::CC_Reg_Base);
					//DPRINTF(FUsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum);
					//DPRINTF(FUsREG,  "		 	: %s\n", os.str());

			}
		}


	static void
		printRegNameBranchs(std::ostringstream &regs_str3, TheISA::RegIndex reg, bool isSource, const MinorDynInst* inst) //const MinorDynInstPtr inst)
		{
			std::string funcName="nothing";
			Addr sym_addr;
			debugSymbolTable->findNearestSymbol(inst->pc.instAddr() , funcName, sym_addr);
			std::ostringstream os;
			//os <<"  " << inst->staticInst->disassemble(0)<<":";

			RegClass reg_class = regIdxToClass(reg);
			std::string preFix;

			if(isSource) preFix="Src";
			else  preFix="Des";

			switch (reg_class)
			{
				case MiscRegClass:
					{
						TheISA::RegIndex misc_reg = reg - TheISA::Misc_Reg_Base;

						/* This is an ugly test because not all archs. have miscRegName */
#if THE_ISA == ARM_ISA
						os << preFix << "M:" << misc_reg << '(' << TheISA::miscRegName[misc_reg] <<
							')';
						DPRINTF(BranchsREG,  "       %s: %s:%s:lastInst_BranchREG= %s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum, regs_str3.str());
						DPRINTF(BranchsREG,  "		 	: %s:lastInst_BranchREG=%s\n", os.str(), os.str() );
#else
						os << preFix << "N:" << misc_reg;
						DPRINTF(BranchsREG,  "       %s: %s:%s:lastInst_BranchREG=%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum,regs_str3.str());
						DPRINTF(BranchsREG,  "		 	: %s:lastInst_BranchREG=%s\n", os.str(),regs_str3.str());
#endif
					}
					break;
				case FloatRegClass:
					//regID=static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);
					{
					os << preFix << "F:" << static_cast<unsigned int>(reg - TheISA::FP_Reg_Base);
					DPRINTF(BranchsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum, regs_str3.str());
					DPRINTF(BranchsREG,  "		 	: %s\n", os.str() ,regs_str3.str() );
					}
					break;
				case IntRegClass:
					//regID=static_cast<unsigned int>(reg);
					if (reg == TheISA::ZeroReg) {

						os << preFix << "Z:" << static_cast<unsigned int>(reg);
						DPRINTF(BranchsREG,  "       %s: %s:%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum, regs_str3.str());
						DPRINTF(BranchsREG,  "		 	: %s\n", os.str(),regs_str3.str());
					} else {

						os << preFix << "X:" << static_cast<unsigned int>(reg);
						DPRINTF(BranchsREG,  "       %s: %s:%s:lastInst_BranchREG=%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum, regs_str3.str());
						DPRINTF(BranchsREG,  "		 	: %s:lastInst_BranchREG=%s\n", os.str(), regs_str3.str());
					}
					break;
				case CCRegClass:
					{
					os << preFix << "C:" << static_cast<unsigned int>(reg - TheISA::CC_Reg_Base);
					DPRINTF(BranchsREG,  "       %s: %s:%s:lastInst_BranchREG=%s\n", funcName,inst->staticInst->disassemble(0), inst->id.execSeqNum, regs_str3.str());
					DPRINTF(BranchsREG,  "		 	: %s:lastInst_BranchREG=%s\n", os.str(), regs_str3.str());
					}

			}
		}


void
MinorDynInst::minorTraceInst(const Named &named_object) const
{
	if (isFault()) {
		MINORINST(&named_object, "id=F;%s addr=0x%x fault=\"%s\"\n",
				id, pc.instAddr(), fault->name());
	} else {
		unsigned int num_src_regs = staticInst->numSrcRegs();
		unsigned int num_dest_regs = staticInst->numDestRegs();

		std::ostringstream regs_str;

		/* Format lists of src and dest registers for microops and
		 *  'full' instructions */
		if (!staticInst->isMacroop()) {
			regs_str << " srcRegs=";

			unsigned int src_reg = 0;
			while (src_reg < num_src_regs) {
				printRegName(regs_str, staticInst->srcRegIdx(src_reg));

				src_reg++;
				if (src_reg != num_src_regs)
					regs_str << ',';
			}

			regs_str << " destRegs=";

			unsigned int dest_reg = 0;
			while (dest_reg < num_dest_regs) {
				printRegName(regs_str, staticInst->destRegIdx(dest_reg));

				dest_reg++;
				if (dest_reg != num_dest_regs)
					regs_str << ',';
			}

#if THE_ISA == ARM_ISA
			regs_str << " extMachInst=" << std::hex << std::setw(16)
				<< std::setfill('0') << staticInst->machInst << std::dec;
#endif
		}

		std::ostringstream flags;
		staticInst->printFlags(flags, " ");

		MINORINST(&named_object, "id=%s addr=0x%x inst=\"%s\" class=%s"
				" flags=\"%s\"%s%s\n",
				id, pc.instAddr(),
				(staticInst->opClass() == No_OpClass ?
				 "(invalid)" : staticInst->disassemble(0,NULL)),
				Enums::OpClassStrings[staticInst->opClass()],
				flags.str(),
				regs_str.str(),
				(predictedTaken ? " predictedTaken" : ""));
	}
}



void
MinorDynInst::minorRegAccess() const
{
	std::string funcName="nothing";
	Addr sym_addr;
	debugSymbolTable->findNearestSymbol(this->pc.instAddr() , funcName, sym_addr);
	if (funcName == "main" || (funcName[0] == 'F' &&  funcName[1] == 'U' && funcName[2] == 'N' && funcName[3] == 'C'))
	{
		//DPRINTF(RegFileAccess,  "In function %s:Inst:%s\n", funcName, this->staticInst->disassemble(0));
		//DPRINTF(RegFileAccess,  "In function %s\n", funcName);

		unsigned int num_src_regs = staticInst->numSrcRegs();
		unsigned int num_dest_regs = staticInst->numDestRegs();

		std::ostringstream regs_str;

		/* Format lists of src and dest registers for microops and
		 *  'full' instructions */
		if (!staticInst->isMacroop()) {
			//  regs_str << " srcRegs:";

			unsigned int src_reg = 0;
			while (src_reg < num_src_regs && (src_reg < 4)) {
				printRegNameminorRegAccess(regs_str, staticInst->srcRegIdx(src_reg), true, this);

				src_reg++;
				// if (src_reg != num_src_regs)
				// regs_str << ':';


			}

			//  regs_str << " destRegs:";

			unsigned int dest_reg = 0;
			while ((dest_reg < num_dest_regs) && (dest_reg < 2)) {
				printRegNameminorRegAccess(regs_str, staticInst->destRegIdx(dest_reg), false, this);


				dest_reg++;
				// if (dest_reg != num_dest_regs)
				//   regs_str << ':';


				//std::ostream &operator <<(std::ostream &os, const MinorDynInst &inst);
				//std::cout<<&(regs_str,this)<<"\n";


			}
		}
	}

}


void
MinorDynInst::minorFUregs() const
{
	std::string funcName="nothing";
	Addr sym_addr;
	debugSymbolTable->findNearestSymbol(this->pc.instAddr() , funcName, sym_addr);
	if (funcName == "main" || (funcName[0] == 'F' &&  funcName[1] == 'U' && funcName[2] == 'N' && funcName[3] == 'C'))
	{
		//DPRINTF(RegFileAccess,  "In function %s:Inst:%s\n", funcName, this->staticInst->disassemble(0));
		//DPRINTF(RegFileAccess,  "In function %s\n", funcName);
		std::ostringstream regs_str;

		/* Format lists of src and dest registers for microops and
		 *  'full' instructions */
		if (!staticInst->isMacroop()) {
			if(staticInst->isLoad())
			{
				printRegNameFUs(regs_str, staticInst->srcRegIdx(0), true, this);
			}
			else if (staticInst->isStore())
			{
				printRegNameFUs(regs_str, staticInst->srcRegIdx(0), true, this);
			}
			else if ((staticInst->isControl()|| staticInst->isCC() || staticInst->isCall()) && staticInst->numSrcRegs())
			{
				printRegNameFUs(regs_str, staticInst->srcRegIdx(0), true, this);
			}
			else
			{
				if( staticInst->srcRegIdx(0) == TheISA::ZeroReg ) //ZERO REG
{
int i=1;
while(staticInst->srcRegIdx(i) == TheISA::ZeroReg && i < staticInst->numSrcRegs())
i++;
					printRegNameFUs(regs_str, staticInst->srcRegIdx(i), true, this);
}
				else
					printRegNameFUs(regs_str, staticInst->srcRegIdx(0), true, this);
			}


		}				
	}
}



void
MinorDynInst::minorBranchregs(MinorDynInstPtr lastInstBranchREG) const
{
std::ostringstream regs_str2;
	std::string funcName="nothing";
	Addr sym_addr;
	debugSymbolTable->findNearestSymbol(this->pc.instAddr() , funcName, sym_addr);
	if (funcName == "main" || (funcName[0] == 'F' &&  funcName[1] == 'U' && funcName[2] == 'N' && funcName[3] == 'C'))
	{
		//DPRINTF(RegFileAccess,  "In function %s:Inst:%s\n", funcName, this->staticInst->disassemble(0));
		//DPRINTF(RegFileAccess,  "In function %s\n", funcName);


		/* Format lists of src and dest registers for microops and
		 *  'full' instructions */
if ( staticInst->getName() == "subs" || staticInst->getName() == "ands" || staticInst->getName() == "adds" || staticInst->getName() == "cmp" || staticInst->getName() == "cmps")
{
DPRINTF(CMPsREG,  "In function %s:Inst:%s:seqNUm:%s\n", funcName, this->staticInst->disassemble(0),this->id.execSeqNum);

}
////////////
//if (!(staticInst->isControl()))
//lastInst_BranchREGNULL;
regs_str2 << lastInstBranchREG->staticInst->disassemble(0);
// t = "mmm";//staticInst->disassemble(0);
/////////////////////
		if (!staticInst->isMacroop()) {
			if ((staticInst->isControl()|| staticInst->isCC() || staticInst->isCall() || staticInst->isUncondCtrl() || staticInst->isDirectCtrl() || staticInst->isReturn() || staticInst->isCondCtrl()) && staticInst->numSrcRegs())
			{

			unsigned int src_reg = 0;
			unsigned int num_src_regs = staticInst->numSrcRegs();
			while (src_reg < num_src_regs) {
					printRegNameBranchs(regs_str2, staticInst->srcRegIdx(src_reg), true, this);
					src_reg++;
				}
			}



		}				
	}
}
MinorDynInst::~MinorDynInst()
{
	if (traceData)
		delete traceData;
}

}
