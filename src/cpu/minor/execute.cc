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
#include "cpu/reg_class.hh"
#include "arch/locked_mem.hh"
#include "arch/registers.hh"
#include "arch/utility.hh"
#include "cpu/minor/cpu.hh"
#include "cpu/minor/exec_context.hh"
#include "cpu/minor/execute.hh"
#include "cpu/minor/fetch1.hh"
#include "cpu/minor/lsq.hh"
#include "cpu/op_class.hh"
#include "debug/Activity.hh"
#include "debug/Branch.hh"
#include "debug/Drain.hh"
#include "debug/MinorExecute.hh"
#include "debug/MinorInterrupt.hh"
#include "debug/MinorMem.hh"
#include "debug/MinorTrace.hh"
#include "debug/PCEvent.hh"
#include "debug/TickMain.hh"
#include "debug/faultInjectionTrack.hh"
#include "debug/RegPointerFI.hh"
#include "debug/FUsREGfaultInjectionTrack.hh"
#include "debug/MainPCs.hh"
#include "debug/printCF.hh"
#include "debug/PCFaultInjectionTrack.hh"
#include "debug/BranchsREGfaultInjectionTrack.hh"
#include "debug/CMPsREGfaultInjectionTrack.hh"
#include "debug/UnnecInst.hh"

namespace Minor
{

	Execute::Execute(const std::string &name_,
			MinorCPU &cpu_,
			MinorCPUParams &params,
			Latch<ForwardInstData>::Output inp_,
			Latch<BranchData>::Input out_) :
		Named(name_),
		inp(inp_),
		out(out_),
		cpu(cpu_),
		issueLimit(params.executeIssueLimit),
		memoryIssueLimit(params.executeMemoryIssueLimit),
		commitLimit(params.executeCommitLimit),
		memoryCommitLimit(params.executeMemoryCommitLimit),
		processMoreThanOneInput(params.executeCycleInput),
		fuDescriptions(*params.executeFuncUnits),
		numFuncUnits(fuDescriptions.funcUnits.size()),
		setTraceTimeOnCommit(params.executeSetTraceTimeOnCommit),
		setTraceTimeOnIssue(params.executeSetTraceTimeOnIssue),
		allowEarlyMemIssue(params.executeAllowEarlyMemoryIssue),
		noCostFUIndex(fuDescriptions.funcUnits.size() + 1),
		lsq(name_ + ".lsq", name_ + ".dcache_port",
				cpu_, *this,
				params.executeMaxAccessesInMemory,
				params.executeMemoryWidth,
				params.executeLSQRequestsQueueSize,
				params.executeLSQTransfersQueueSize,
				params.executeLSQStoreBufferSize,
				params.executeLSQMaxStoreBufferStoresPerCycle),
		scoreboard(name_ + ".scoreboard"),
		FItarget(params.FItarget), //Fault injection
		FItargetReg(params.FItargetReg), //Fault injection
		MaxTick(params.MaxTick), //Fault injection
		enableSWIFT(params.enableSWIFTR),
		enableZDC(params.enableZDCR),
		inputBuffer(name_ + ".inputBuffer", "insts",
				params.executeInputBufferSize),
		inputIndex(0),
		lastCommitWasEndOfMacroop(true),
		instsBeingCommitted(params.executeCommitLimit),
		streamSeqNum(InstId::firstStreamSeqNum),
		lastPredictionSeqNum(InstId::firstPredictionSeqNum),
		drainState(NotDraining)
	{
		if (commitLimit < 1) {
			fatal("%s: executeCommitLimit must be >= 1 (%d)\n", name_,
					commitLimit);
		}

		if (issueLimit < 1) {
			fatal("%s: executeCommitLimit must be >= 1 (%d)\n", name_,
					issueLimit);
		}

		if (memoryIssueLimit < 1) {
			fatal("%s: executeMemoryIssueLimit must be >= 1 (%d)\n", name_,
					memoryIssueLimit);
		}

		if (memoryCommitLimit > commitLimit) {
			fatal("%s: executeMemoryCommitLimit (%d) must be <="
					" executeCommitLimit (%d)\n",
					name_, memoryCommitLimit, commitLimit);
		}

		if (params.executeInputBufferSize < 1) {
			fatal("%s: executeInputBufferSize must be >= 1 (%d)\n", name_,
					params.executeInputBufferSize);
		}

		if (params.executeInputBufferSize < 1) {
			fatal("%s: executeInputBufferSize must be >= 1 (%d)\n", name_,
					params.executeInputBufferSize);
		}

		/* This should be large enough to count all the in-FU instructions
		 *  which need to be accounted for in the inFlightInsts
		 *  queue */
		unsigned int total_slots = 0;

		/* Make FUPipelines for each MinorFU */
		for (unsigned int i = 0; i < numFuncUnits; i++) {
			std::ostringstream fu_name;
			MinorFU *fu_description = fuDescriptions.funcUnits[i];

			/* Note the total number of instruction slots (for sizing
			 *  the inFlightInst queue) and the maximum latency of any FU
			 *  (for sizing the activity recorder) */
			total_slots += fu_description->opLat;

			fu_name << name_ << ".fu." << i;

			FUPipeline *fu = new FUPipeline(fu_name.str(), *fu_description, cpu);

			funcUnits.push_back(fu);
		}

		/** Check that there is a functional unit for all operation classes */
		for (int op_class = No_OpClass + 1; op_class < Num_OpClass; op_class++) {
			bool found_fu = false;
			unsigned int fu_index = 0;

			while (fu_index < numFuncUnits && !found_fu)
			{
				if (funcUnits[fu_index]->provides(
							static_cast<OpClass>(op_class)))
				{
					found_fu = true;
				}
				fu_index++;
			}

			if (!found_fu) {
				warn("No functional unit for OpClass %s\n",
						Enums::OpClassStrings[op_class]);
			}
		}

		inFlightInsts = new Queue<QueuedInst,
			      ReportTraitsAdaptor<QueuedInst> >(
					      name_ + ".inFlightInsts", "insts", total_slots);

		inFUMemInsts = new Queue<QueuedInst,
			     ReportTraitsAdaptor<QueuedInst> >(
					     name_ + ".inFUMemInsts", "insts", total_slots);
	}

	const ForwardInstData *
		Execute::getInput()
		{
			/* Get a line from the inputBuffer to work with */
			if (!inputBuffer.empty()) {
				const ForwardInstData &head = inputBuffer.front();

				return (head.isBubble() ? NULL : &(inputBuffer.front()));
			} else {
				return NULL;
			}
		}

	void
		Execute::popInput()
		{
			if (!inputBuffer.empty())
				inputBuffer.pop();

			inputIndex = 0;
		}

	void
		Execute::tryToBranch(MinorDynInstPtr inst, Fault fault, BranchData &branch)
		{
			ThreadContext *thread = cpu.getContext(inst->id.threadId);
			const TheISA::PCState &pc_before = inst->pc;
			TheISA::PCState target = thread->pcState();

			/* Force a branch for SerializeAfter instructions at the end of micro-op
			 *  sequence when we're not suspended */
			bool force_branch = thread->status() != ThreadContext::Suspended &&
				!inst->isFault() &&
				inst->isLastOpInInst() &&
				(inst->staticInst->isSerializeAfter() ||
				 inst->staticInst->isIprAccess());

			DPRINTF(Branch, "tryToBranch before: %s after: %s%s\n",
					pc_before, target, (force_branch ? " (forcing)" : ""));

			/* Will we change the PC to something other than the next instruction? */
			bool must_branch = pc_before != target ||
				fault != NoFault ||
				force_branch;

			/* The reason for the branch data we're about to generate, set below */
			BranchData::Reason reason = BranchData::NoBranch;

			if (fault == NoFault)
			{
				if (!(inst->staticInst->isControl()) )
					lastInst = inst;
				///////////working area for fault injection on PC-ssssssssss





				std::string funcName="nothing";
				Addr sym_addr;
				debugSymbolTable->findNearestSymbol(inst->pc.instAddr() , funcName, sym_addr);
				if(inst->staticInst->isControl() && ( (funcName[0] == 'F' &&  funcName[1] == 'U' && funcName[2] == 'N' && funcName[3] == 'C') || funcName == "main") )
					DPRINTF(MainPCs, "Func: %s Inst: %s PC:%s:----LastInt:%s\n", funcName, inst->staticInst->disassemble(0), inst->pc.instAddr(),lastInst->staticInst->disassemble(0));




				///////////working area for fault injection on PC
				if( !faultIsInjected && (FItargetReg == 1001) && (curTick() == FItarget) && must_branch )
				{

					faultIsInjected=true;
					srand (time(0));
					int randBit = rand()%500;

					DPRINTF(PCFaultInjectionTrack, "FUNC:%s	Inst:%s: True Pc of Inst was PC:%s\n",funcName, inst->staticInst->disassemble(0), target.instAddr());
					while(randBit)
					{
						TheISA::advancePC(target, inst->staticInst);
						randBit--;
					}

				}
				/////////////////////////////
				/////////////////////////////-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee
				TheISA::advancePC(target, inst->staticInst);
				thread->pcState(target);
				////////////s
				if(faultIsInjected && (FItargetReg == 1001) && (curTick() == FItarget) && must_branch)
					DPRINTF(PCFaultInjectionTrack, "FUNC:%s Inst:%s: Faulty Pc of Inst is PC:%s\n",funcName, inst->staticInst->disassemble(0), target.instAddr() );
				if(faultIsInjected && (FItargetReg == 1001) && (curTick() <= FItarget + 100000 ))
					DPRINTF(PCFaultInjectionTrack, "Funct: %s Following Inst:%s: PC:%s\n",funcName, inst->staticInst->disassemble(0),inst->pc.instAddr());
				//////////////e
				//branch register fault injection SSSS
				headOfInFlightInst = inst->id.execSeqNum;
				if(!test && FItarget == headOfInFlightInst && BranchsFI)
				{
					DPRINTF(BranchsREGfaultInjectionTrack, "FUNC= %s\nTarget instruction for Branch fault injection is %s\n",funcName, inst->staticInst->disassemble(0));
					test=true;
				}

				///eeee
				//CMP register fault injection SSSS

				if(!test && FItarget == headOfInFlightInst && CMPsFI)
				{
					DPRINTF(CMPsREGfaultInjectionTrack, "FUNC= %s\nTarget instruction for CMP fault injection is %s\n",funcName, inst->staticInst->disassemble(0));
					test=true;
				}

				///eeee


				DPRINTF(Branch, "Advancing current PC from: %s to: %s\n",
						pc_before, target);
			}

			if (inst->predictedTaken && !force_branch) {
				/* Predicted to branch */
				if (!must_branch) {
					/* No branch was taken, change stream to get us back to the
					 *  intended PC value */
					DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x but"
							" none happened inst: %s\n",
							inst->pc.instAddr(), inst->predictedTarget.instAddr(), *inst);

					reason = BranchData::BadlyPredictedBranch;
				} else if (inst->predictedTarget == target) {
					/* Branch prediction got the right target, kill the branch and
					 *  carry on.
					 *  Note that this information to the branch predictor might get
					 *  overwritten by a "real" branch during this cycle */
					DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x correctly"
							" inst: %s\n",
							inst->pc.instAddr(), inst->predictedTarget.instAddr(), *inst);

					reason = BranchData::CorrectlyPredictedBranch;
				} else {
					/* Branch prediction got the wrong target */
					DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x"
							" but got the wrong target (actual: 0x%x) inst: %s\n",
							inst->pc.instAddr(), inst->predictedTarget.instAddr(),
							target.instAddr(), *inst);

					reason = BranchData::BadlyPredictedBranchTarget;
				}
			} else if (must_branch) {
				/* Unpredicted branch */
				DPRINTF(Branch, "Unpredicted branch from 0x%x to 0x%x inst: %s\n",
						inst->pc.instAddr(), target.instAddr(), *inst);

				reason = BranchData::UnpredictedBranch;
			} else {
				/* No branch at all */
				reason = BranchData::NoBranch;
			}

			updateBranchData(reason, inst, target, branch);
		}

	void
		Execute::updateBranchData(
				BranchData::Reason reason,
				MinorDynInstPtr inst, const TheISA::PCState &target,
				BranchData &branch)
		{
			if (reason != BranchData::NoBranch) {
				/* Bump up the stream sequence number on a real branch*/
				if (BranchData::isStreamChange(reason))
					streamSeqNum++;

				/* Branches (even mis-predictions) don't change the predictionSeqNum,
				 *  just the streamSeqNum */
				branch = BranchData(reason, streamSeqNum,
						/* Maintaining predictionSeqNum if there's no inst is just a
						 * courtesy and looks better on minorview */
						(inst->isBubble() ? lastPredictionSeqNum
						 : inst->id.predictionSeqNum),
						target, inst);

				DPRINTF(Branch, "Branch data signalled: %s\n", branch);
			}
		}

	void
		Execute::handleMemResponse(MinorDynInstPtr inst,
				LSQ::LSQRequestPtr response, BranchData &branch, Fault &fault)
		{
			ThreadID thread_id = inst->id.threadId;
			ThreadContext *thread = cpu.getContext(thread_id);

			ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

			PacketPtr packet = response->packet;

			bool is_load = inst->staticInst->isLoad();
			bool is_store = inst->staticInst->isStore();
			bool is_prefetch = inst->staticInst->isDataPrefetch();

			/* If true, the trace's predicate value will be taken from the exec
			 *  context predicate, otherwise, it will be set to false */
			bool use_context_predicate = true;

			if (response->fault != NoFault) {
				/* Invoke memory faults. */
				DPRINTF(MinorMem, "Completing fault from DTLB access: %s\n",
						response->fault->name());

				if (inst->staticInst->isPrefetch()) {
					DPRINTF(MinorMem, "Not taking fault on prefetch: %s\n",
							response->fault->name());

					/* Don't assign to fault */
				} else {
					/* Take the fault raised during the TLB/memory access */
					fault = response->fault;

					fault->invoke(thread, inst->staticInst);
				}
			} else if (!packet) {
				DPRINTF(MinorMem, "Completing failed request inst: %s\n",
						*inst);
				use_context_predicate = false;
			} else if (packet->isError()) {
				DPRINTF(MinorMem, "Trying to commit error response: %s\n",
						*inst);

				fatal("Received error response packet for inst: %s\n", *inst);
			} else if (is_store || is_load || is_prefetch) {
				assert(packet);

				DPRINTF(MinorMem, "Memory response inst: %s addr: 0x%x size: %d\n",
						*inst, packet->getAddr(), packet->getSize());

				if (is_load && packet->getSize() > 0) {
					DPRINTF(MinorMem, "Memory data[0]: 0x%x\n",
							static_cast<unsigned int>(packet->getConstPtr<uint8_t>()[0]));
				}

				/* Complete the memory access instruction */
				fault = inst->staticInst->completeAcc(packet, &context,
						inst->traceData);

				if (fault != NoFault) {
					/* Invoke fault created by instruction completion */
					DPRINTF(MinorMem, "Fault in memory completeAcc: %s\n",
							fault->name());
					fault->invoke(thread, inst->staticInst);
				} else {
					/* Stores need to be pushed into the store buffer to finish
					 *  them off */
					if (response->needsToBeSentToStoreBuffer())
						lsq.sendStoreToStoreBuffer(response);
				}
			} else {
				fatal("There should only ever be reads, "
						"writes or faults at this point\n");
			}

			lsq.popResponse(response);

			if (inst->traceData) {
				inst->traceData->setPredicate((use_context_predicate ?
							context.readPredicate() : false));
			}

			doInstCommitAccounting(inst);


			/* Generate output to account for branches */
			tryToBranch(inst, fault, branch);
		}

	bool
		Execute::isInterrupted(ThreadID thread_id) const
		{
			return cpu.checkInterrupts(cpu.getContext(thread_id));
		}

	bool
		Execute::takeInterrupt(ThreadID thread_id, BranchData &branch)
		{
			DPRINTF(MinorInterrupt, "Considering interrupt status from PC: %s\n",
					cpu.getContext(thread_id)->pcState());

			Fault interrupt = cpu.getInterruptController()->getInterrupt
				(cpu.getContext(thread_id));

			if (interrupt != NoFault) {
				/* The interrupt *must* set pcState */
				cpu.getInterruptController()->updateIntrInfo
					(cpu.getContext(thread_id));
				interrupt->invoke(cpu.getContext(thread_id));

				assert(!lsq.accessesInFlight());

				DPRINTF(MinorInterrupt, "Invoking interrupt: %s to PC: %s\n",
						interrupt->name(), cpu.getContext(thread_id)->pcState());

				/* Assume that an interrupt *must* cause a branch.  Assert this? */

				updateBranchData(BranchData::Interrupt, MinorDynInst::bubble(),
						cpu.getContext(thread_id)->pcState(), branch);
			}

			return interrupt != NoFault;
		}

	bool
		Execute::executeMemRefInst(MinorDynInstPtr inst, BranchData &branch,
				bool &passed_predicate, Fault &fault)
		{
			bool issued = false;

			/* Set to true if the mem op. is issued and sent to the mem system */
			passed_predicate = false;

			if (!lsq.canRequest()) {
				/* Not acting on instruction yet as the memory
				 * queues are full */
				issued = false;
			} else {
				ThreadContext *thread = cpu.getContext(inst->id.threadId);
				TheISA::PCState old_pc = thread->pcState();

				ExecContext context(cpu, *cpu.threads[inst->id.threadId],
						*this, inst);

				DPRINTF(MinorExecute, "Initiating memRef inst: %s\n", *inst);

				Fault init_fault = inst->staticInst->initiateAcc(&context,
						inst->traceData);

				if (init_fault != NoFault) {
					DPRINTF(MinorExecute, "Fault on memory inst: %s"
							" initiateAcc: %s\n", *inst, init_fault->name());
					fault = init_fault;
				} else {
					/* Only set this if the instruction passed its
					 * predicate */
					passed_predicate = context.readPredicate();

					/* Set predicate in tracing */
					if (inst->traceData)
						inst->traceData->setPredicate(passed_predicate);

					/* If the instruction didn't pass its predicate (and so will not
					 *  progress from here)  Try to branch to correct and branch
					 *  mis-prediction. */
					if (!passed_predicate) {
						/* Leave it up to commit to handle the fault */
						lsq.pushFailedRequest(inst);
					}
				}

				/* Restore thread PC */
				thread->pcState(old_pc);
				issued = true;
			}

			return issued;
		}

	/** Increment a cyclic buffer index for indices [0, cycle_size-1] */
	inline unsigned int
		cyclicIndexInc(unsigned int index, unsigned int cycle_size)
		{
			unsigned int ret = index + 1;

			if (ret == cycle_size)
				ret = 0;

			return ret;
		}

	/** Decrement a cyclic buffer index for indices [0, cycle_size-1] */
	inline unsigned int
		cyclicIndexDec(unsigned int index, unsigned int cycle_size)
		{
			int ret = index - 1;

			if (ret < 0)
				ret = cycle_size - 1;

			return ret;
		}

	unsigned int
		Execute::issue(bool only_issue_microops)
		{
			const ForwardInstData *insts_in = getInput();

			/* Early termination if we have no instructions */
			if (!insts_in)
				return 0;

			/* Start from the first FU */
			unsigned int fu_index = 0;

			/* Remains true while instructions are still being issued.  If any
			 *  instruction fails to issue, this is set to false and we exit issue.
			 *  This strictly enforces in-order issue.  For other issue behaviours,
			 *  a more complicated test in the outer while loop below is needed. */
			bool issued = true;

			/* Number of insts issues this cycle to check for issueLimit */
			unsigned num_insts_issued = 0;

			/* Number of memory ops issues this cycle to check for memoryIssueLimit */
			unsigned num_mem_insts_issued = 0;

			/* Number of instructions discarded this cycle in order to enforce a
			 *  discardLimit. @todo, add that parameter? */
			unsigned num_insts_discarded = 0;

			do {
				MinorDynInstPtr inst = insts_in->insts[inputIndex];
				ThreadID thread_id = inst->id.threadId;
				Fault fault = inst->fault;
				bool discarded = false;
				bool issued_mem_ref = false;

				if (inst->isBubble()) {
					/* Skip */
					issued = true;
				} else if (cpu.getContext(thread_id)->status() ==
						ThreadContext::Suspended)
				{
					DPRINTF(MinorExecute, "Not issuing inst: %s from suspended"
							" thread\n", *inst);

					issued = false;
				} else if (inst->id.streamSeqNum != streamSeqNum) {
					DPRINTF(MinorExecute, "Discarding inst: %s as its stream"
							" state was unexpected, expected: %d\n",
							*inst, streamSeqNum);
					issued = true;
					discarded = true;
				} else if (fault == NoFault && only_issue_microops &&
						/* Is this anything other than a non-first microop */
						(!inst->staticInst->isMicroop() ||
						 !inst->staticInst->isFirstMicroop()))
				{
					DPRINTF(MinorExecute, "Not issuing new non-microop inst: %s\n",
							*inst);

					issued = false;
				} else {
					/* Try and issue an instruction into an FU, assume we didn't and
					 * fix that in the loop */
					issued = false;

					/* Try FU from 0 each instruction */
					fu_index = 0;

					/* Try and issue a single instruction stepping through the
					 *  available FUs */
					do {
						FUPipeline *fu = funcUnits[fu_index];

						DPRINTF(MinorExecute, "Trying to issue inst: %s to FU: %d\n",
								*inst, fu_index);

						/* Does the examined fu have the OpClass-related capability
						 *  needed to execute this instruction?  Faults can always
						 *  issue to any FU but probably should just 'live' in the
						 *  inFlightInsts queue rather than having an FU. */
						bool fu_is_capable = (!inst->isFault() ?
								fu->provides(inst->staticInst->opClass()) : true);

						if (inst->isNoCostInst()) {
							/* Issue free insts. to a fake numbered FU */
							fu_index = noCostFUIndex;

							/* And start the countdown on activity to allow
							 *  this instruction to get to the end of its FU */
							cpu.activityRecorder->activity();

							/* Mark the destinations for this instruction as
							 *  busy */
							scoreboard.markupInstDests(inst, cpu.curCycle() +
									Cycles(0), cpu.getContext(thread_id), false, ScoreboardFI, FItarget);

							inst->fuIndex = noCostFUIndex;
							inst->extraCommitDelay = Cycles(0);
							inst->extraCommitDelayExpr = NULL;

							/* Push the instruction onto the inFlight queue so
							 *  it can be committed in order */
							QueuedInst fu_inst(inst);
							inFlightInsts->push(fu_inst);

							issued = true;

						} else if (!fu_is_capable || fu->alreadyPushed()) {
							/* Skip */
							if (!fu_is_capable) {
								DPRINTF(MinorExecute, "Can't issue as FU: %d isn't"
										" capable\n", fu_index);
							} else {
								DPRINTF(MinorExecute, "Can't issue as FU: %d is"
										" already busy\n", fu_index);
							}
						} else if (fu->stalled) {
							DPRINTF(MinorExecute, "Can't issue inst: %s into FU: %d,"
									" it's stalled\n",
									*inst, fu_index);
						} else if (!fu->canInsert()) {
							DPRINTF(MinorExecute, "Can't issue inst: %s to busy FU"
									" for another: %d cycles\n",
									*inst, fu->cyclesBeforeInsert());
						} else {
							MinorFUTiming *timing = (!inst->isFault() ?
									fu->findTiming(inst->staticInst) : NULL);

							const std::vector<Cycles> *src_latencies =
								(timing ? &(timing->srcRegsRelativeLats)
								 : NULL);

							const std::vector<bool> *cant_forward_from_fu_indices =
								&(fu->cantForwardFromFUIndices);

							if (timing && timing->suppress) {
								DPRINTF(MinorExecute, "Can't issue inst: %s as extra"
										" decoding is suppressing it\n",
										*inst);
							} else if (!scoreboard.canInstIssue(inst, src_latencies,
										cant_forward_from_fu_indices,
										cpu.curCycle(), cpu.getContext(thread_id)))
							{
								DPRINTF(MinorExecute, "Can't issue inst: %s yet\n",
										*inst);
							} else {
								/* Can insert the instruction into this FU */
								DPRINTF(MinorExecute, "Issuing inst: %s"
										" into FU %d\n", *inst,
										fu_index);

								Cycles extra_dest_retire_lat = Cycles(0);
								TimingExpr *extra_dest_retire_lat_expr = NULL;
								Cycles extra_assumed_lat = Cycles(0);

								/* Add the extraCommitDelay and extraAssumeLat to
								 *  the FU pipeline timings */
								if (timing) {
									extra_dest_retire_lat =
										timing->extraCommitLat;
									extra_dest_retire_lat_expr =
										timing->extraCommitLatExpr;
									extra_assumed_lat =
										timing->extraAssumedLat;
								}

								issued_mem_ref = inst->isMemRef();

								QueuedInst fu_inst(inst);

								/* Decorate the inst with FU details */
								inst->fuIndex = fu_index;
								inst->extraCommitDelay = extra_dest_retire_lat;
								inst->extraCommitDelayExpr =
									extra_dest_retire_lat_expr;

								if (issued_mem_ref) {
									/* Remember which instruction this memory op
									 *  depends on so that initiateAcc can be called
									 *  early */
									if (allowEarlyMemIssue) {
										inst->instToWaitFor =
											scoreboard.execSeqNumToWaitFor(inst,
													cpu.getContext(thread_id));

										if (lsq.getLastMemBarrier() >
												inst->instToWaitFor)
										{
											DPRINTF(MinorExecute, "A barrier will"
													" cause a delay in mem ref issue of"
													" inst: %s until after inst"
													" %d(exec)\n", *inst,
													lsq.getLastMemBarrier());

											inst->instToWaitFor =
												lsq.getLastMemBarrier();
										} else {
											DPRINTF(MinorExecute, "Memory ref inst:"
													" %s must wait for inst %d(exec)"
													" before issuing\n",
													*inst, inst->instToWaitFor);
										}

										inst->canEarlyIssue = true;
									}
									/* Also queue this instruction in the memory ref
									 *  queue to ensure in-order issue to the LSQ */
									DPRINTF(MinorExecute, "Pushing mem inst: %s\n",
											*inst);
									inFUMemInsts->push(fu_inst);
								}

								/* Issue to FU */
								fu->push(fu_inst);
								/* And start the countdown on activity to allow
								 *  this instruction to get to the end of its FU */
								cpu.activityRecorder->activity();

								/* Mark the destinations for this instruction as
								 *  busy */
								scoreboard.markupInstDests(inst, cpu.curCycle() +
										fu->description.opLat +
										extra_dest_retire_lat +
										extra_assumed_lat,
										cpu.getContext(thread_id),
										issued_mem_ref && extra_assumed_lat == Cycles(0), ScoreboardFI, FItarget);

								/* Push the instruction onto the inFlight queue so
								 *  it can be committed in order */
								inFlightInsts->push(fu_inst);

								issued = true;
							}
						}

						fu_index++;
					} while (fu_index != numFuncUnits && !issued);

					if (!issued)
						DPRINTF(MinorExecute, "Didn't issue inst: %s\n", *inst);
				}

				if (issued) {
					/* Generate MinorTrace's MinorInst lines.  Do this at commit
					 *  to allow better instruction annotation? */
					if (DTRACE(MinorTrace) && !inst->isBubble())
						inst->minorTraceInst(*this);

					/* Mark up barriers in the LSQ */
					if (!discarded && inst->isInst() &&
							inst->staticInst->isMemBarrier())
					{
						DPRINTF(MinorMem, "Issuing memory barrier inst: %s\n", *inst);
						lsq.issuedMemBarrierInst(inst);
					}

					if (inst->traceData && setTraceTimeOnIssue) {
						inst->traceData->setWhen(curTick());
					}

					if (issued_mem_ref)
						num_mem_insts_issued++;

					if (discarded) {
						num_insts_discarded++;
					} else {
						num_insts_issued++;

						if (num_insts_issued == issueLimit)
							DPRINTF(MinorExecute, "Reached inst issue limit\n");
					}

					inputIndex++;
					DPRINTF(MinorExecute, "Stepping to next inst inputIndex: %d\n",
							inputIndex);
				}

				/* Got to the end of a line */
				if (inputIndex == insts_in->width()) {
					popInput();
					/* Set insts_in to null to force us to leave the surrounding
					 *  loop */
					insts_in = NULL;

					if (processMoreThanOneInput) {
						DPRINTF(MinorExecute, "Wrapping\n");
						insts_in = getInput();
					}
				}
			} while (insts_in && inputIndex < insts_in->width() &&
					/* We still have instructions */
					fu_index != numFuncUnits && /* Not visited all FUs */
					issued && /* We've not yet failed to issue an instruction */
					num_insts_issued != issueLimit && /* Still allowed to issue */
					num_mem_insts_issued != memoryIssueLimit);

			return num_insts_issued;
		}

	bool
		Execute::tryPCEvents()
		{
			ThreadContext *thread = cpu.getContext(0);
			unsigned int num_pc_event_checks = 0;

			/* Handle PC events on instructions */
			Addr oldPC;
			do {
				oldPC = thread->instAddr();
				cpu.system->pcEventQueue.service(thread);
				num_pc_event_checks++;
			} while (oldPC != thread->instAddr());

			if (num_pc_event_checks > 1) {
				DPRINTF(PCEvent, "Acting on PC Event to PC: %s\n",
						thread->pcState());
			}

			return num_pc_event_checks > 1;
		}
	bool Execute::inMain(MinorDynInstPtr inst)
	{
		std::string funcName="nothing";
		Addr sym_addr;
		debugSymbolTable->findNearestSymbol(inst->pc.instAddr() , funcName, sym_addr);
		if(( (funcName[0] == 'F' &&  funcName[1] == 'U' && funcName[2] == 'N' && funcName[3] == 'C') || funcName == "main") )
			return true;
		return false;

	}

	enum Aarch64 {X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, X29, X30, SP=43 , XZR=31 };
	bool Execute::isSWIFTMasterReg(int reg)
	{
		if (reg == Aarch64::X0 || reg == Aarch64::X1 || reg == Aarch64::X2 || reg == Aarch64::X19 || reg == Aarch64::X20 || reg == Aarch64::X23 || reg == Aarch64::X24 || reg == Aarch64::X29 || reg == Aarch64::X30 || reg == Aarch64::SP) return true;
		return false;
	}
	bool Execute::isSWIFTSlaveReg(int reg)
	{
		if (isSWIFTMasterReg(reg)) return false;
		return true;
	}
	bool Execute::isZDCMasterReg(int reg)
	{
		if (reg == Aarch64::X0 || reg == Aarch64::X1 || reg == Aarch64::X2 || reg == Aarch64::X3 || reg == Aarch64::X4 || reg == Aarch64::X5 || reg == Aarch64::X19 || reg == Aarch64::X20 || reg == Aarch64::X23 || reg == Aarch64::X24 || reg == Aarch64::X28 ||  reg == Aarch64::X29 || reg == Aarch64::X30 || reg == Aarch64::SP) return true;
		return false;
	}
	bool Execute::isZDCSlaveReg(int reg)
	{
		if (isZDCMasterReg(reg)) return false;
		return true;
	}
	bool Execute::isUnnecessaryInst(MinorDynInstPtr inst)
	{
		bool desIsSlave=false;
		bool srcIsMaster=false;
		bool oneSrcIsZR=false;
		if (enableSWIFT && inMain(inst) ){
			unsigned int num_src_regs = inst->staticInst->numSrcRegs();
			unsigned int num_dest_regs = inst->staticInst->numDestRegs();
			unsigned int src_reg = 0, des_reg = 0;
			while (des_reg < num_dest_regs  && (inst->staticInst->getName() == "sub") ) {
				if (isSWIFTSlaveReg(inst->staticInst->destRegIdx(des_reg)) && inst->staticInst->destRegIdx(des_reg) < 32) {desIsSlave=true; }
				des_reg++;
			}
			while (src_reg < num_src_regs  && (inst->staticInst->getName() == "sub") ) {
				if (isSWIFTMasterReg(inst->staticInst->srcRegIdx(src_reg)) && (inst->staticInst->srcRegIdx(src_reg) < 32 || inst->staticInst->srcRegIdx(src_reg) == Aarch64::SP)) {srcIsMaster=true; }
				if (inst->staticInst->srcRegIdx(src_reg) == Aarch64::XZR) {oneSrcIsZR=true; }
				src_reg++;
			}


		}
		if (enableZDC && inMain(inst) ){
			unsigned int num_src_regs = inst->staticInst->numSrcRegs();
			unsigned int num_dest_regs = inst->staticInst->numDestRegs();
			unsigned int src_reg = 0, des_reg = 0;
			while (des_reg < num_dest_regs  && (inst->staticInst->getName() == "sub") ) {
				if (isZDCSlaveReg(inst->staticInst->destRegIdx(des_reg)) && inst->staticInst->destRegIdx(des_reg) < 32) {desIsSlave=true; }
				des_reg++;
			}
			while (src_reg < num_src_regs  && (inst->staticInst->getName() == "sub")) {
				if (isZDCMasterReg(inst->staticInst->srcRegIdx(src_reg)) && (inst->staticInst->srcRegIdx(src_reg) < 32 || inst->staticInst->srcRegIdx(src_reg) == Aarch64::SP)) {srcIsMaster=true; }
				if (inst->staticInst->srcRegIdx(src_reg) == Aarch64::XZR) {oneSrcIsZR=true; }
				src_reg++;
			}


		}
		if (desIsSlave && srcIsMaster && oneSrcIsZR )
			return true;
		return false;

	}

	void
		Execute::doInstCommitAccounting(MinorDynInstPtr inst)
		{
////for fault injection debugging purpose
if(!faultGetsMasked && inMain(inst) && faultIsInjected && 0)
DPRINTF(faultInjectionTrack, "%s \n",inst->staticInst->disassemble(0));



			assert(!inst->isFault());
			//print instruction sources and destinations in main
			inst->minorRegAccess();
			/////////////////////////////////////////////////////
			//print instruction results in FUs for FI on FU
			inst->minorFUregs();

			if (!(inst->staticInst->isControl()))
				lastInstBranchREG = inst;

			inst->minorBranchregs(lastInstBranchREG);


			/*
			   std::string funcName="nothing";
			   Addr sym_addr;
			   debugSymbolTable->findNearestSymbol(cpu.getContext(0)->instAddr() , funcName, sym_addr);
			   if (funcName == "main")
			   {
			//inst->dump();
			std::cout << "Inst: "<< inst->staticInst->disassemble(0) << "\n";
			for (int desReg=0; desReg < inst->staticInst->numDestRegs(); desReg++)
			{
			if( regIdxToClass(inst->staticInst->destRegIdx(desReg)) == FloatRegClass )
			std::cout << "destRegIdx of destination register(" <<desReg<<") is" << inst->staticInst->destRegIdx(desReg)-TheISA::FP_Reg_Base<<"\n";
			}

			for (int srcReg=0; srcReg < inst->staticInst->numSrcRegs(); srcReg++)
			{
			if( regIdxToClass(inst->staticInst->srcRegIdx(srcReg)) == FloatRegClass )
			std::cout << "srcRegIdx of source register(" <<srcReg<<") is" << inst->staticInst->srcRegIdx(srcReg)-TheISA::FP_Reg_Base<<"\n";
			}


			}
			 */

			MinorThread *thread = cpu.threads[inst->id.threadId];

			/* Increment the many and various inst and op counts in the
			 *  thread and system */
			if (!inst->staticInst->isMicroop() || inst->staticInst->isLastMicroop())
			{
				thread->numInst++;
				thread->numInsts++;
				cpu.stats.numInsts++;
				if((enableSWIFT || enableZDC) && isUnnecessaryInst(inst))
					{
					cpu.stats.numUnnecessaryInst++;
					DPRINTF(UnnecInst, "%s\n", inst->staticInst->disassemble(0));
					}
			}
			thread->numOp++;
			thread->numOps++;
			cpu.stats.numOps++;
			cpu.system->totalNumInsts++;



			/* Act on events related to instruction counts */
			cpu.comInstEventQueue[inst->id.threadId]->serviceEvents(thread->numInst);
			cpu.system->instEventQueue.serviceEvents(cpu.system->totalNumInsts);

			/* Set the CP SeqNum to the numOps commit number */
			if (inst->traceData)
				inst->traceData->setCPSeq(thread->numOp);

			cpu.probeInstCommit(inst->staticInst);
		}

	bool
		Execute::commitInst(MinorDynInstPtr inst, bool early_memory_issue,
				BranchData &branch, Fault &fault, bool &committed,
				bool &completed_mem_issue)
		{
			ThreadID thread_id = inst->id.threadId;
			ThreadContext *thread = cpu.getContext(thread_id);

			bool completed_inst = true;
			fault = NoFault;

			/* Is the thread for this instruction suspended?  In that case, just
			 *  stall as long as there are no pending interrupts */
			if (thread->status() == ThreadContext::Suspended &&
					!isInterrupted(thread_id))
			{
				DPRINTF(MinorExecute, "Not committing inst from suspended thread"
						" inst: %s\n", *inst);
				completed_inst = false;
			} else if (inst->isFault()) {
				ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

				DPRINTF(MinorExecute, "Fault inst reached Execute: %s\n",
						inst->fault->name());

				fault = inst->fault;
				inst->fault->invoke(thread, NULL);

				tryToBranch(inst, fault, branch);
			} else if (inst->staticInst->isMemRef()) {
				/* Memory accesses are executed in two parts:
				 *  executeMemRefInst -- calculates the EA and issues the access
				 *      to memory.  This is done here.
				 *  handleMemResponse -- handles the response packet, done by
				 *      Execute::commit
				 *
				 *  While the memory access is in its FU, the EA is being
				 *  calculated.  At the end of the FU, when it is ready to
				 *  'commit' (in this function), the access is presented to the
				 *  memory queues.  When a response comes back from memory,
				 *  Execute::commit will commit it.
				 */
				bool predicate_passed = false;
				bool completed_mem_inst = executeMemRefInst(inst, branch,
						predicate_passed, fault);

				if (completed_mem_inst && fault != NoFault) {
					if (early_memory_issue) {
						DPRINTF(MinorExecute, "Fault in early executing inst: %s\n",
								fault->name());
						/* Don't execute the fault, just stall the instruction
						 *  until it gets to the head of inFlightInsts */
						inst->canEarlyIssue = false;
						/* Not completed as we'll come here again to pick up
						 *  the fault when we get to the end of the FU */
						completed_inst = false;
					} else {
						DPRINTF(MinorExecute, "Fault in execute: %s\n",
								fault->name());
						fault->invoke(thread, NULL);

						tryToBranch(inst, fault, branch);
						completed_inst = true;
					}
				} else {
					completed_inst = completed_mem_inst;
				}
				completed_mem_issue = completed_inst;
			} else if (inst->isInst() && inst->staticInst->isMemBarrier() &&
					!lsq.canPushIntoStoreBuffer())
			{
				DPRINTF(MinorExecute, "Can't commit data barrier inst: %s yet as"
						" there isn't space in the store buffer\n", *inst);

				completed_inst = false;
			} else {
				ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

				DPRINTF(MinorExecute, "Committing inst: %s\n", *inst);

				fault = inst->staticInst->execute(&context,
						inst->traceData);

				/* Set the predicate for tracing and dump */
				if (inst->traceData)
					inst->traceData->setPredicate(context.readPredicate());

				committed = true;

				if (fault != NoFault) {
					DPRINTF(MinorExecute, "Fault in execute of inst: %s fault: %s\n",
							*inst, fault->name());
					fault->invoke(thread, inst->staticInst);
				}

				doInstCommitAccounting(inst);
				tryToBranch(inst, fault, branch);
			}

			if (completed_inst) {
				/* Keep a copy of this instruction's predictionSeqNum just in case
				 * we need to issue a branch without an instruction (such as an
				 * interrupt) */
				lastPredictionSeqNum = inst->id.predictionSeqNum;

				/* Check to see if this instruction suspended the current thread. */
				if (!inst->isFault() &&
						thread->status() == ThreadContext::Suspended &&
						branch.isBubble() && /* It didn't branch too */
						!isInterrupted(thread_id)) /* Don't suspend if we have
									      interrupts */
				{
					TheISA::PCState resume_pc = cpu.getContext(0)->pcState();

					assert(resume_pc.microPC() == 0);

					DPRINTF(MinorInterrupt, "Suspending thread: %d from Execute"
							" inst: %s\n", inst->id.threadId, *inst);

					cpu.stats.numFetchSuspends++;

					updateBranchData(BranchData::SuspendThread, inst, resume_pc,
							branch);
				}
			}

			return completed_inst;
		}

	void
		Execute::commit(bool only_commit_microops, bool discard, BranchData &branch)
		{
			Fault fault = NoFault;
			Cycles now = cpu.curCycle();

			/**
			 * Try and execute as many instructions from the end of FU pipelines as
			 *  possible.  This *doesn't* include actually advancing the pipelines.
			 *
			 * We do this by looping on the front of the inFlightInsts queue for as
			 *  long as we can find the desired instruction at the end of the
			 *  functional unit it was issued to without seeing a branch or a fault.
			 *  In this function, these terms are used:
			 *      complete -- The instruction has finished its passage through
			 *          its functional unit and its fate has been decided
			 *          (committed, discarded, issued to the memory system)
			 *      commit -- The instruction is complete(d), not discarded and has
			 *          its effects applied to the CPU state
			 *      discard(ed) -- The instruction is complete but not committed
			 *          as its streamSeqNum disagrees with the current
			 *          Execute::streamSeqNum
			 *
			 *  Commits are also possible from two other places:
			 *
			 *  1) Responses returning from the LSQ
			 *  2) Mem ops issued to the LSQ ('committed' from the FUs) earlier
			 *      than their position in the inFlightInsts queue, but after all
			 *      their dependencies are resolved.
			 */

			/* Has an instruction been completed?  Once this becomes false, we stop
			 *  trying to complete instructions. */
			bool completed_inst = true;

			/* Number of insts committed this cycle to check against commitLimit */
			unsigned int num_insts_committed = 0;

			/* Number of memory access instructions committed to check against
			 *  memCommitLimit */
			unsigned int num_mem_refs_committed = 0;

			if (only_commit_microops && !inFlightInsts->empty()) {
				DPRINTF(MinorInterrupt, "Only commit microops %s %d\n",
						*(inFlightInsts->front().inst),
						lastCommitWasEndOfMacroop);
			}

			while (!inFlightInsts->empty() && /* Some more instructions to process */
					!branch.isStreamChange() && /* No real branch */
					fault == NoFault && /* No faults */
					completed_inst && /* Still finding instructions to execute */
					num_insts_committed != commitLimit /* Not reached commit limit */
			      )
			{
				if (only_commit_microops) {
					DPRINTF(MinorInterrupt, "Committing tail of insts before"
							" interrupt: %s\n",
							*(inFlightInsts->front().inst));
				}

				QueuedInst *head_inflight_inst = &(inFlightInsts->front());

				InstSeqNum head_exec_seq_num =
					head_inflight_inst->inst->id.execSeqNum;

				/* The instruction we actually process if completed_inst
				 *  remains true to the end of the loop body.
				 *  Start by considering the the head of the in flight insts queue */
				MinorDynInstPtr inst = head_inflight_inst->inst;
				//if (!(inst->staticInst->isControl()))
				//lastInst_BranchREG = inst;
				/////////////////fault injection of pipeline registers
				std::string funcName="nothing";
				Addr sym_addr;
				debugSymbolTable->findNearestSymbol(head_inflight_inst->inst->pc.instAddr() , funcName, sym_addr);
				headOfInFlightInst = head_inflight_inst->inst->id.execSeqNum;
//moslem
//head_inflight_inst->inst->staticInst->debugEnd = FItarget + 100000;
				if(!test && FItarget == headOfInFlightInst && FUsFI)
				{
					DPRINTF(RegPointerFI, "FUNC= %s\nTarget instruction for pipeline registers fault injection is %s\n",funcName, head_inflight_inst->inst->staticInst->disassemble(0));
					DPRINTF(FUsREGfaultInjectionTrack, "From execute.cc---FUNC= %s\nTarget instruction for FUs fault injection is %s\n",funcName, head_inflight_inst->inst->staticInst->disassemble(0));
					test=true;
				}
				///////////////////
				/////////////////fault injection of branches registers

				else if(!test && FItarget == headOfInFlightInst && BranchsFI)
				{
					DPRINTF(BranchsREGfaultInjectionTrack, "FUNC= %s\nTarget instruction for Branch fault injection is %s\n",funcName, head_inflight_inst->inst->staticInst->disassemble(0));
					test=true;
				}
				///////////////////

				bool committed_inst = false;
				bool discard_inst = false;
				bool completed_mem_ref = false;
				bool issued_mem_ref = false;
				bool early_memory_issue = false;

				/* Must set this again to go around the loop */
				completed_inst = false;

				/* If we're just completing a macroop before an interrupt or drain,
				 *  can we stil commit another microop (rather than a memory response)
				 *  without crosing into the next full instruction? */
				bool can_commit_insts = !inFlightInsts->empty() &&
					!(only_commit_microops && lastCommitWasEndOfMacroop);

				/* Can we find a mem response for this inst */
				LSQ::LSQRequestPtr mem_response =
					(inst->inLSQ ? lsq.findResponse(inst) : NULL);

				DPRINTF(MinorExecute, "Trying to commit canCommitInsts: %d\n",
						can_commit_insts);

				/* Test for PC events after every instruction */
				if (isInbetweenInsts() && tryPCEvents()) {
					ThreadContext *thread = cpu.getContext(0);

					/* Branch as there was a change in PC */
					updateBranchData(BranchData::UnpredictedBranch,
							MinorDynInst::bubble(), thread->pcState(), branch);
				} else if (mem_response &&
						num_mem_refs_committed < memoryCommitLimit)
				{
					/* Try to commit from the memory responses next */
					discard_inst = inst->id.streamSeqNum != streamSeqNum ||
						discard;

					DPRINTF(MinorExecute, "Trying to commit mem response: %s\n",
							*inst);

					/* Complete or discard the response */
					if (discard_inst) {
						DPRINTF(MinorExecute, "Discarding mem inst: %s as its"
								" stream state was unexpected, expected: %d\n",
								*inst, streamSeqNum);

						lsq.popResponse(mem_response);
					} else {
						handleMemResponse(inst, mem_response, branch, fault);
						committed_inst = true;


					}

					completed_mem_ref = true;
					completed_inst = true;
				} else if (can_commit_insts) {
					/* If true, this instruction will, subject to timing tweaks,
					 *  be considered for completion.  try_to_commit flattens
					 *  the `if' tree a bit and allows other tests for inst
					 *  commit to be inserted here. */
					bool try_to_commit = false;

					/* Try and issue memory ops early if they:
					 *  - Can push a request into the LSQ
					 *  - Have reached the end of their FUs
					 *  - Have had all their dependencies satisfied
					 *  - Are from the right stream
					 *
					 *  For any other case, leave it to the normal instruction
					 *  issue below to handle them.
					 */
					if (!inFUMemInsts->empty() && lsq.canRequest()) {
						DPRINTF(MinorExecute, "Trying to commit from mem FUs\n");

						const MinorDynInstPtr head_mem_ref_inst =
							inFUMemInsts->front().inst;
						FUPipeline *fu = funcUnits[head_mem_ref_inst->fuIndex];
						const MinorDynInstPtr &fu_inst = fu->front().inst;

						/* Use this, possibly out of order, inst as the one
						 *  to 'commit'/send to the LSQ */
						if (!fu_inst->isBubble() &&
								!fu_inst->inLSQ &&
								fu_inst->canEarlyIssue &&
								streamSeqNum == fu_inst->id.streamSeqNum &&
								head_exec_seq_num > fu_inst->instToWaitFor)
						{
							DPRINTF(MinorExecute, "Issuing mem ref early"
									" inst: %s instToWaitFor: %d\n",
									*(fu_inst), fu_inst->instToWaitFor);

							inst = fu_inst;
							try_to_commit = true;
							early_memory_issue = true;
							completed_inst = true;
							/*
							//// Working area for fault injection on LSQ
							//LSQ::LSQRequestPtr mem_response =
							//(inst->inLSQ ? lsq.findResponse(inst) : NULL);
							std::string funcName="nothing";
							Addr sym_addr;
							debugSymbolTable->findNearestSymbol(cpu.getContext(0)->instAddr() , funcName, sym_addr);
							if (funcName == "main" )
							{

							std::cout << "Inst: "<< fu_inst->staticInst->disassemble(0) << "   SeqNum: " << fu_inst->id.execSeqNum << "\n";
							//std::cout << "Data: "<< mem_response->data << "\n";
							}

							 */
							///////////////////////////////////////////////
						}
					}

					/* Try and commit FU-less insts */
					if (!completed_inst && inst->isNoCostInst()) {
						DPRINTF(MinorExecute, "Committing no cost inst: %s", *inst);

						try_to_commit = true;
						completed_inst = true;
					}

					/* Try to issue from the ends of FUs and the inFlightInsts
					 *  queue */
					if (!completed_inst && !inst->inLSQ) {
						DPRINTF(MinorExecute, "Trying to commit from FUs\n");

						/* Try to commit from a functional unit */
						/* Is the head inst of the expected inst's FU actually the
						 *  expected inst? */
						QueuedInst &fu_inst =
							funcUnits[inst->fuIndex]->front();
						InstSeqNum fu_inst_seq_num = fu_inst.inst->id.execSeqNum;

						if (fu_inst.inst->isBubble()) {
							/* No instruction ready */
							completed_inst = false;
						} else if (fu_inst_seq_num != head_exec_seq_num) {
							/* Past instruction: we must have already executed it
							 * in the same cycle and so the head inst isn't
							 * actually at the end of its pipeline
							 * Future instruction: handled above and only for
							 * mem refs on their way to the LSQ */
						} else /* if (fu_inst_seq_num == head_exec_seq_num) */ {
							/* All instructions can be committed if they have the
							 *  right execSeqNum and there are no in-flight
							 *  mem insts before us */
							try_to_commit = true;
							completed_inst = true;
						}
					}

					if (try_to_commit) {
						discard_inst = inst->id.streamSeqNum != streamSeqNum ||
							discard;

						/* Is this instruction discardable as its streamSeqNum
						 *  doesn't match? */
						if (!discard_inst) {
							/* Try to commit or discard a non-memory instruction.
							 *  Memory ops are actually 'committed' from this FUs
							 *  and 'issued' into the memory system so we need to
							 *  account for them later (commit_was_mem_issue gets
							 *  set) */
							if (inst->extraCommitDelayExpr) {
								DPRINTF(MinorExecute, "Evaluating expression for"
										" extra commit delay inst: %s\n", *inst);

								ThreadContext *thread =
									cpu.getContext(inst->id.threadId);

								TimingExprEvalContext context(inst->staticInst,
										thread, NULL);

								uint64_t extra_delay = inst->extraCommitDelayExpr->
									eval(context);

								DPRINTF(MinorExecute, "Extra commit delay expr"
										" result: %d\n", extra_delay);

								if (extra_delay < 128) {
									inst->extraCommitDelay += Cycles(extra_delay);
								} else {
									DPRINTF(MinorExecute, "Extra commit delay was"
											" very long: %d\n", extra_delay);
								}
								inst->extraCommitDelayExpr = NULL;
							}

							/* Move the extraCommitDelay from the instruction
							 *  into the minimumCommitCycle */
							if (inst->extraCommitDelay != Cycles(0)) {
								inst->minimumCommitCycle = cpu.curCycle() +
									inst->extraCommitDelay;
								inst->extraCommitDelay = Cycles(0);
							}

							/* @todo Think about making lastMemBarrier be
							 *  MAX_UINT_64 to avoid using 0 as a marker value */
							if (!inst->isFault() && inst->isMemRef() &&
									lsq.getLastMemBarrier() <
									inst->id.execSeqNum &&
									lsq.getLastMemBarrier() != 0)
							{
								DPRINTF(MinorExecute, "Not committing inst: %s yet"
										" as there are incomplete barriers in flight\n",
										*inst);
								completed_inst = false;
							} else if (inst->minimumCommitCycle > now) {
								DPRINTF(MinorExecute, "Not committing inst: %s yet"
										" as it wants to be stalled for %d more cycles\n",
										*inst, inst->minimumCommitCycle - now);
								completed_inst = false;
							} else {
								completed_inst = commitInst(inst,
										early_memory_issue, branch, fault,
										committed_inst, issued_mem_ref);
							}
						} else {
							/* Discard instruction */
							completed_inst = true;
						}

						if (completed_inst) {
							/* Allow the pipeline to advance.  If the FU head
							 *  instruction wasn't the inFlightInsts head
							 *  but had already been committed, it would have
							 *  unstalled the pipeline before here */
							if (inst->fuIndex != noCostFUIndex)
								funcUnits[inst->fuIndex]->stalled = false;
						}
					}
				} else {
					DPRINTF(MinorExecute, "No instructions to commit\n");
					completed_inst = false;
				}

				/* All discardable instructions must also be 'completed' by now */
				assert(!(discard_inst && !completed_inst));

				/* Instruction committed but was discarded due to streamSeqNum
				 *  mismatch */
				if (discard_inst) {
					DPRINTF(MinorExecute, "Discarding inst: %s as its stream"
							" state was unexpected, expected: %d\n",
							*inst, streamSeqNum);

					if (fault == NoFault)
						cpu.stats.numDiscardedOps++;
				}

				/* Mark the mem inst as being in the LSQ */
				if (issued_mem_ref) {
					inst->fuIndex = 0;
					inst->inLSQ = true;
				}

				/* Pop issued (to LSQ) and discarded mem refs from the inFUMemInsts
				 *  as they've *definitely* exited the FUs */
				if (completed_inst && inst->isMemRef()) {
					/* The MemRef could have been discarded from the FU or the memory
					 *  queue, so just check an FU instruction */
					if (!inFUMemInsts->empty() &&
							inFUMemInsts->front().inst == inst)
					{
						inFUMemInsts->pop();
					}
				}

				if (completed_inst && !(issued_mem_ref && fault == NoFault)) {
					/* Note that this includes discarded insts */
					DPRINTF(MinorExecute, "Completed inst: %s\n", *inst);

					/* Got to the end of a full instruction? */
					lastCommitWasEndOfMacroop = inst->isFault() ||
						inst->isLastOpInInst();

					/* lastPredictionSeqNum is kept as a convenience to prevent its
					 *  value from changing too much on the minorview display */
					lastPredictionSeqNum = inst->id.predictionSeqNum;

					/* Finished with the inst, remove it from the inst queue and
					 *  clear its dependencies */
					inFlightInsts->pop();

					/* Complete barriers in the LSQ/move to store buffer */
					if (inst->isInst() && inst->staticInst->isMemBarrier()) {
						DPRINTF(MinorMem, "Completing memory barrier"
								" inst: %s committed: %d\n", *inst, committed_inst);
						lsq.completeMemBarrierInst(inst, committed_inst);
					}

					scoreboard.clearInstDests(inst, inst->isMemRef());
				}

				/* Handle per-cycle instruction counting */
				if (committed_inst) {
					bool is_no_cost_inst = inst->isNoCostInst();

					/* Don't show no cost instructions as having taken a commit
					 *  slot */
					if (DTRACE(MinorTrace) && !is_no_cost_inst)
						instsBeingCommitted.insts[num_insts_committed] = inst;

					if (!is_no_cost_inst)
						num_insts_committed++;

					if (num_insts_committed == commitLimit)
						DPRINTF(MinorExecute, "Reached inst commit limit\n");

					/* Re-set the time of the instruction if that's required for
					 * tracing */
					if (inst->traceData) {
						if (setTraceTimeOnCommit)
							inst->traceData->setWhen(curTick());
						inst->traceData->dump();
					}

					if (completed_mem_ref)
						num_mem_refs_committed++;

					if (num_mem_refs_committed == memoryCommitLimit)
						DPRINTF(MinorExecute, "Reached mem ref commit limit\n");
				}
			}
		}

	bool
		Execute::isInbetweenInsts() const
		{
			return lastCommitWasEndOfMacroop &&
				!lsq.accessesInFlight();
		}

	void
		Execute::evaluate()
		{
			inputBuffer.setTail(*inp.outputWire);
			BranchData &branch = *out.inputWire;

			const ForwardInstData *insts_in = getInput();

			/* Do all the cycle-wise activities for dcachePort here to potentially
			 *  free up input spaces in the LSQ's requests queue */
			lsq.step();

			/* Has an interrupt been signalled?  This may not be acted on
			 *  straighaway so this is different from took_interrupt below */
			bool interrupted = false;
			/* If there was an interrupt signalled, was it acted on now? */
			bool took_interrupt = false;

			if (cpu.getInterruptController()) {
				/* This is here because it seems that after drainResume the
				 * interrupt controller isn't always set */
				interrupted = drainState == NotDraining && isInterrupted(0);
			} else {
				DPRINTF(MinorInterrupt, "No interrupt controller\n");
			}

			unsigned int num_issued = 0;

			if (DTRACE(MinorTrace)) {
				/* Empty the instsBeingCommitted for MinorTrace */
				instsBeingCommitted.bubbleFill();
			}

			/* THREAD threadId on isInterrupted */
			/* Act on interrupts */
			if (interrupted && isInbetweenInsts()) {
				took_interrupt = takeInterrupt(0, branch);
				/* Clear interrupted if no interrupt was actually waiting */
				interrupted = took_interrupt;
			}

			if (took_interrupt) {
				/* Do no commit/issue this cycle */
			} else if (!branch.isBubble()) {
				/* It's important that this is here to carry Fetch1 wakeups to Fetch1
				 *  without overwriting them */
				DPRINTF(MinorInterrupt, "Execute skipping a cycle to allow old"
						" branch to complete\n");
			} else {
				if (interrupted) {
					if (inFlightInsts->empty()) {
						DPRINTF(MinorInterrupt, "Waiting but no insts\n");
					} else {
						DPRINTF(MinorInterrupt, "Waiting for end of inst before"
								" signalling interrupt\n");
					}
				}

				/* commit can set stalled flags observable to issue and so *must* be
				 *  called first */
				if (drainState != NotDraining) {
					if (drainState == DrainCurrentInst) {
						/* Commit only micro-ops, don't kill anything else */
						commit(true, false, branch);

						if (isInbetweenInsts())
							setDrainState(DrainHaltFetch);

						/* Discard any generated branch */
						branch = BranchData::bubble();
					} else if (drainState == DrainAllInsts) {
						/* Kill all instructions */
						while (getInput())
							popInput();
						commit(false, true, branch);
					}
				} else {
					/* Commit micro-ops only if interrupted.  Otherwise, commit
					 *  anything you like */
					commit(interrupted, false, branch);
				}

				/* This will issue merrily even when interrupted in the sure and
				 *  certain knowledge that the interrupt with change the stream */
				if (insts_in)
					num_issued = issue(false);
			}

			/* Halt fetch, but don't do it until we have the current instruction in
			 *  the bag */
			if (drainState == DrainHaltFetch) {
				updateBranchData(BranchData::HaltFetch, MinorDynInst::bubble(),
						TheISA::PCState(0), branch);

				cpu.wakeupOnEvent(Pipeline::ExecuteStageId);
				setDrainState(DrainAllInsts);
			}

			MinorDynInstPtr next_issuable_inst = NULL;
			bool can_issue_next = false;

			/* Find the next issuable instruction and see if it can be issued */
			if (getInput()) {
				MinorDynInstPtr inst = getInput()->insts[inputIndex];

				if (inst->isFault()) {
					can_issue_next = true;
				} else if (!inst->isBubble()) {
					if (cpu.getContext(inst->id.threadId)->status() !=
							ThreadContext::Suspended)
					{
						next_issuable_inst = inst;
					}
				}
			}

			bool becoming_stalled = true;

			/* Advance the pipelines and note whether they still need to be
			 *  advanced */
			for (unsigned int i = 0; i < numFuncUnits; i++) {
				FUPipeline *fu = funcUnits[i];

				fu->advance();

				/* If we need to go again, the pipeline will have been left or set
				 *  to be unstalled */
				if (fu->occupancy != 0 && !fu->stalled)
					becoming_stalled = false;

				/* Could we possibly issue the next instruction?  This is quite
				 *  an expensive test */
				if (next_issuable_inst && !fu->stalled &&
						scoreboard.canInstIssue(next_issuable_inst,
							NULL, NULL, cpu.curCycle() + Cycles(1),
							cpu.getContext(next_issuable_inst->id.threadId)) &&
						fu->provides(next_issuable_inst->staticInst->opClass()))
				{
					can_issue_next = true;
				}
			}

			bool head_inst_might_commit = false;

			/* Could the head in flight insts be committed */
			if (!inFlightInsts->empty()) {
				const QueuedInst &head_inst = inFlightInsts->front();

				if (head_inst.inst->isNoCostInst()) {
					head_inst_might_commit = true;
				} else {
					FUPipeline *fu = funcUnits[head_inst.inst->fuIndex];

					/* Head inst is commitable */
					if ((fu->stalled &&
								fu->front().inst->id == head_inst.inst->id) ||
							lsq.findResponse(head_inst.inst))
					{
						head_inst_might_commit = true;

					}
				}
			}

			DPRINTF(Activity, "Need to tick num issued insts: %s%s%s%s%s%s\n",
					(num_issued != 0 ? " (issued some insts)" : ""),
					(becoming_stalled ? " (becoming stalled)" : "(not becoming stalled)"),
					(can_issue_next ? " (can issued next inst)" : ""),
					(head_inst_might_commit ? "(head inst might commit)" : ""),
					(lsq.needsToTick() ? " (LSQ needs to tick)" : ""),
					(interrupted ? " (interrupted)" : ""));

			bool need_to_tick =
				num_issued != 0 || /* Issued some insts this cycle */
				!becoming_stalled || /* Some FU pipelines can still move */
				can_issue_next || /* Can still issue a new inst */
				head_inst_might_commit || /* Could possible commit the next inst */
				lsq.needsToTick() || /* Must step the dcache port */
				interrupted; /* There are pending interrupts */

			if (!need_to_tick) {
				DPRINTF(Activity, "The next cycle might be skippable as there are no"
						" advanceable FUs\n");
			}

			/* Wake up if we need to tick again */
			if (need_to_tick)
				cpu.wakeupOnEvent(Pipeline::ExecuteStageId);

			/* Note activity of following buffer */
			if (!branch.isBubble())
				cpu.activityRecorder->activity();

			/* Make sure the input (if any left) is pushed */
			inputBuffer.pushTail();

			////////////////Fault injection: get the main tickes////////////////////////////////////////////////////////
			//bool inMain=false;
			std::string sym_str="nothing";
			Addr sym_addr;
			debugSymbolTable->findNearestSymbol(cpu.getContext(0)->instAddr() , sym_str, sym_addr);

			if ( /*(sym_str[0] == 'F' &&  sym_str[1] == 'U' && sym_str[2] == 'N' && sym_str[3] == 'C' )*/ !insertedTomain && sym_str == "main" ){
				//inMain=true;

				insertedTomain=true;
				funcName=sym_str;
			}
			if (MaxTick && (curTick() > MaxTick))
			{
				DPRINTF(faultInjectionTrack, "Some this is wrong! Execution takes too long");
				if (FItarget) 
				{
					fatal("%s: EXIT, too long!!! \n",curTick() );
				}
			}

			if(insertedTomain && ((sym_str[0] == 'F' &&  sym_str[1] == 'U' && sym_str[2] == 'N' && sym_str[3] == 'C') || sym_str == "main" ))
			{

				//////
				// moslem for printing out the program control flow
				if ( funcName !=  lastPlace)
				{
					DPRINTF(printCF, "%d -> %s \n",counter, funcName);
					counter++;
					lastPlace=funcName;
				}
				///////////////////

				DPRINTF(TickMain, "FunctionaName:=%s\n",sym_str );
				cpu.stats.tickCyclesMain++;
				funcName=sym_str;
				///// dead interval evalution
				int numberInstinIQ=inputBuffer.getSizeBuffer();
				int numberEntriesinLSQ=lsq.numValidEntriesInLSQQueues();

				if (need_to_tick || true)
				{
					FUPipeline *fu0 = funcUnits[0];
					if(fu0->alreadyPushed() || !fu0->canInsert() || fu0->stalled)
						cpu.stats.FU0isBusy++;
					FUPipeline *fu1 = funcUnits[1];
					if(fu1->alreadyPushed() || !fu1->canInsert() || fu1->stalled)
						cpu.stats.FU1isBusy++;
					FUPipeline *fu2 = funcUnits[2];
					if(fu2->alreadyPushed() || !fu2->canInsert() || fu2->stalled)
						cpu.stats.FU2isBusy++;
					FUPipeline *fu3 = funcUnits[3];
					if(fu3->alreadyPushed() || !fu3->canInsert() || fu3->stalled)
						cpu.stats.FU3isBusy++;
					FUPipeline *fu4 = funcUnits[4];
					if(fu4->alreadyPushed() || !fu4->canInsert() || fu4->stalled)
						cpu.stats.FU4isBusy++;
					FUPipeline *fu5 = funcUnits[5];
					if(fu5->alreadyPushed() || !fu5->canInsert() || fu5->stalled)
						cpu.stats.FU5isBusy++;
					FUPipeline *fu6 = funcUnits[6];
					if(fu6->alreadyPushed() || !fu6->canInsert() || fu6->stalled)
						cpu.stats.FU6isBusy++;
					switch (numberInstinIQ)
					{
						case 0:
							{
								cpu.stats.Inst0InIQ++;
								break;
							}
						case 1:
							{
								cpu.stats.Inst1InIQ++;
								break;
							}
						case 2:
							{
								cpu.stats.Inst2InIQ++;
								break;
							}
						case 3:
							{
								cpu.stats.Inst3InIQ++;
								break;
							}
						case 4:
							{
								cpu.stats.Inst4InIQ++;
								break;
							}
						case 5:
							{
								cpu.stats.Inst5InIQ++;
								break;
							}
						case 6:
							{
								cpu.stats.Inst6InIQ++;
								break;
							}
						case 7:
							{
								cpu.stats.Inst7InIQ++;
								break;
							}

					}
					switch (numberEntriesinLSQ)
					{
						case 0:
							{
								cpu.stats.Inst0InLSQ++;
								break;
							}
						case 1:
							{
								cpu.stats.Inst1InLSQ++;
								break;
							}
						case 2:
							{
								cpu.stats.Inst2InLSQ++;
								break;
							}
						case 3:
							{
								cpu.stats.Inst3InLSQ++;
								break;
							}
						case 4:
							{
								cpu.stats.Inst4InLSQ++;
								break;
							}
						case 5:
							{
								cpu.stats.Inst5InLSQ++;
								break;
							}
						case 6:
							{
								cpu.stats.Inst6InLSQ++;
								break;
							}
						case 7:
							{
								cpu.stats.Inst7InLSQ++;
								break;
							}
						case 8:
							{
								cpu.stats.Inst8InLSQ++;
								break;
							}

					}
				}

				/////////////////////

			}


			//////////////////////////Inject fault in register file
			if (FItarget == curTick() && insertedTomain && !faultIsInjected )
			{

				//std::cout << "TheISA::Max_Reg_Index: "<< TheISA::Max_Reg_Index << "\n";
				int randBit,temp=0;
				///
				////////////inject fault on int reg

				if (FItargetReg == 100) //integer regsiter fault injection
				{
					while(!FItargetRegClass)
					{

						srand (time(0));
						FItargetReg = rand()%(NUM_INTREGS); 
						srand (time(0));
						randBit = rand()%62;
						temp = pow (2, randBit);
						if(FItargetReg == 33) FItargetReg = NUM_INTREGS;

						if (((FItargetReg >= 0 && FItargetReg <= NUM_ARCH_INTREGS) || (FItargetReg == NUM_INTREGS)) && (FItargetReg != 31)) FItargetRegClass = regClass::INTEGER; // we just inject faults on 31 GPR and SP
					}
				}
				else if (FItargetReg == 2000)//float register fault injection
				{
					int maxTry=0;
					while(!FItargetRegClass)
					{

						srand (time(0));
						FItargetReg = rand()%(80); 
						srand (time(0));
						randBit = rand()%62;
						temp = pow (2, randBit);
						maxTry++;
						//if ((FItargetReg > TheISA::FP_Reg_Base)) {
						FItargetRegClass = regClass::FLOAT;
						//if (!(cpu.threads[0]->readFloatRegBits(FItargetReg - TheISA::FP_Reg_Base)) || !(maxTry < 100))  FItargetRegClass =0;
						//}
					}
				}
				else if (FItargetReg < 50) //accept register from Input
					{
					randBit = rand()%62;
						temp = pow (2, randBit);
					FItargetRegClass = regClass::INTEGER;
					}
				//std::cout << "NUM_ARCH_INTREGS: " << NUM_ARCH_INTREGS << "\n"; //=32
				//std::cout << "NUM_INTREGS: " << NUM_INTREGS << "\n";//=43
				//std::cout << "TheISA::FP_Reg_Base: " << TheISA::FP_Reg_Base << "\n";//=1376
				//std::cout << "TheISA::CC_Reg_Base: " << TheISA::CC_Reg_Base << "\n";//1536
				//std::cout << "TheISA::Misc_Reg_Base: " << TheISA::Misc_Reg_Base << "\n";//=1542
				//if (reg == ISA::
				////
				if(!FItargetRegClass || true)
				{
					DPRINTF(faultInjectionTrack, "random selected reg(relative): %s\n.", FItargetReg);
					//reg = (cpu.getContext(0))->flattenIntIndex(reg);
					//DPRINTF(faultInjectionTrack, "random selected reg(after flatten): %s\n.", reg);
					bool ret=false;
					long trueValue,faultyValue;
					//std::cout << "TheISA::NumIntRegs" << TheISA::NumIntRegs << "\n";
					//std::cout << "TheISA::NumFloatRegs" << TheISA::NumFloatRegs << "\n";
					switch (FItargetRegClass)
					{
						case regClass::INTEGER:
							//FItargetReg = reg;
							trueValue=cpu.threads[0]->readIntReg(FItargetReg);
							faultyValue=trueValue xor temp;
							cpu.threads[0]->setIntReg(FItargetReg, faultyValue);
							DPRINTF(faultInjectionTrack, "In Function: %s fault is injected on the integer register %s, true value was %s and the fliped bit is %s, so the faulty value is %s\n", funcName, FItargetReg, trueValue, randBit,cpu.threads[0]->readIntReg(FItargetReg));
							ret = true;
							break;
						case regClass::FLOAT:
							//FItargetReg -= TheISA::FP_Reg_Base; //TheISA::NumIntRegs + TheISA::NumCCRegs + FItargetReg - TheISA::FP_Reg_Base - 33;
							trueValue=cpu.threads[0]->readFloatRegBits(FItargetReg);
							faultyValue=trueValue xor temp;
							cpu.threads[0]->setFloatRegBits(FItargetReg, faultyValue);
							DPRINTF(faultInjectionTrack, "In Function: %s fault is injected on the float register %s, true value was %s and the fliped bit is %s, so the faulty value is %s\n", funcName, FItargetReg, trueValue, randBit,cpu.threads[0]->readFloatRegBits(FItargetReg));
							ret = true;
							break;
						case regClass::CC:
							FItargetReg = TheISA::NumIntRegs + FItargetReg - TheISA::FP_Reg_Base;
							trueValue=cpu.threads[0]->readCCReg(FItargetReg);
							faultyValue=trueValue xor temp;
							cpu.threads[0]->setCCReg(FItargetReg, faultyValue);
							DPRINTF(faultInjectionTrack, "In Function: %s fault is injected on the CC register %s, true value was %s and the fliped bit is %s, so the faulty value is %s\n", funcName, FItargetReg, trueValue, randBit,cpu.threads[0]->readIntReg(FItargetReg));
							ret = true;
							break;
						case regClass::MISC:
							/* Don't bother with Misc registers */
							ret = false;
							break;
					}
					if(!ret)
						std::cout << "number is wrong\n";
					else

						faultIsInjected=true;
					//1)select a random register



					//2) determine the regClass


					//3)call the approprate  function for reading the true value

					//4)call the approprate function for setting the faulty value
					//std::cout << "TheISA::Max_Reg_Index: "<< TheISA::Max_Reg_Index << "\n";
					//faultIsInjected=true;
					//ExecContext context(cpu, *cpu.threads[0], *this, inst);
				}
			}
			////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		}

	void
		Execute::wakeupFetch(BranchData::Reason reason)
		{
			BranchData branch;
			assert(branch.isBubble());

			/* THREAD thread id */
			ThreadContext *thread = cpu.getContext(0);

			/* Force a branch to the current PC (which should be the next inst.) to
			 *  wake up Fetch1 */
			if (!branch.isStreamChange() /* No real branch already happened */) {
				DPRINTF(MinorInterrupt, "Waking up Fetch (via Execute) by issuing"
						" a branch: %s\n", thread->pcState());

				assert(thread->pcState().microPC() == 0);

				updateBranchData(reason,
						MinorDynInst::bubble(), thread->pcState(), branch);
			} else {
				DPRINTF(MinorInterrupt, "Already branching, no need for wakeup\n");
			}

			*out.inputWire = branch;

			/* Make sure we get ticked */
			cpu.wakeupOnEvent(Pipeline::ExecuteStageId);
		}

	void
		Execute::minorTrace() const
		{
			std::ostringstream insts;
			std::ostringstream stalled;

			instsBeingCommitted.reportData(insts);
			lsq.minorTrace();
			inputBuffer.minorTrace();
			scoreboard.minorTrace();

			/* Report functional unit stalling in one string */
			unsigned int i = 0;
			while (i < numFuncUnits)
			{
				stalled << (funcUnits[i]->stalled ? '1' : 'E');
				i++;
				if (i != numFuncUnits)
					stalled << ',';
			}

			MINORTRACE("insts=%s inputIndex=%d streamSeqNum=%d"
					" stalled=%s drainState=%d isInbetweenInsts=%d\n",
					insts.str(), inputIndex, streamSeqNum, stalled.str(), drainState,
					isInbetweenInsts());

			std::for_each(funcUnits.begin(), funcUnits.end(),
					std::mem_fun(&FUPipeline::minorTrace));

			inFlightInsts->minorTrace();
			inFUMemInsts->minorTrace();
		}

	void
		Execute::drainResume()
		{
			DPRINTF(Drain, "MinorExecute drainResume\n");

			setDrainState(NotDraining);

			/* Wakeup fetch and keep the pipeline running until that branch takes
			 *  effect */
			wakeupFetch(BranchData::WakeupFetch);
			cpu.wakeupOnEvent(Pipeline::ExecuteStageId);
		}

	std::ostream &operator <<(std::ostream &os, Execute::DrainState state)
	{
		switch (state)
		{
			case Execute::NotDraining:
				os << "NotDraining";
				break;
			case Execute::DrainCurrentInst:
				os << "DrainCurrentInst";
				break;
			case Execute::DrainHaltFetch:
				os << "DrainHaltFetch";
				break;
			case Execute::DrainAllInsts:
				os << "DrainAllInsts";
				break;
			default:
				os << "Drain-" << static_cast<int>(state);
				break;
		}

		return os;
	}

	void
		Execute::setDrainState(DrainState state)
		{
			DPRINTF(Drain, "setDrainState: %s\n", state);
			drainState = state;
		}

	unsigned int
		Execute::drain()
		{
			DPRINTF(Drain, "MinorExecute drain\n");

			if (drainState == NotDraining) {
				cpu.wakeupOnEvent(Pipeline::ExecuteStageId);

				/* Go to DrainCurrentInst if we're between microops
				 * or waiting on an unbufferable memory operation.
				 * Otherwise we can go straight to DrainHaltFetch
				 */
				if (isInbetweenInsts())
					setDrainState(DrainHaltFetch);
				else
					setDrainState(DrainCurrentInst);
			}

			return (isDrained() ? 0 : 1);
		}

	bool
		Execute::isDrained()
		{
			return drainState == DrainAllInsts &&
				inputBuffer.empty() &&
				inFlightInsts->empty() &&
				lsq.isDrained();
		}

	Execute::~Execute()
	{
		for (unsigned int i = 0; i < numFuncUnits; i++)
			delete funcUnits[i];

		delete inFlightInsts;
	}

	bool
		Execute::instIsRightStream(MinorDynInstPtr inst)
		{
			return inst->id.streamSeqNum == streamSeqNum;
		}

	bool
		Execute::instIsHeadInst(MinorDynInstPtr inst)
		{
			bool ret = false;

			if (!inFlightInsts->empty())
				ret = inFlightInsts->front().inst->id == inst->id;

			return ret;
		}

	MinorCPU::MinorCPUPort &
		Execute::getDcachePort()
		{
			return lsq.getDcachePort();
		}

}
