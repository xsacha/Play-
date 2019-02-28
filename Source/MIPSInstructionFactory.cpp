#include <assert.h>
#include <stddef.h>
#include "MIPSInstructionFactory.h"
#include "MIPS.h"
#include "offsetof_def.h"
#include "COP_SCU.h"

CMIPSInstructionFactory::CMIPSInstructionFactory(MIPS_REGSIZE nRegSize)
    : m_regSize(nRegSize)
{
}

void CMIPSInstructionFactory::SetupQuickVariables(uint32 nAddress, CMipsJitter* codeGen, CMIPS* pCtx)
{
	m_pCtx = pCtx;
	m_codeGen = codeGen;
	m_nAddress = nAddress;

	m_nOpcode = m_pCtx->m_pMemoryMap->GetInstruction(m_nAddress);
}

static void HandleTLBError(CMIPS* context)
{
	if(context->m_State.nCOP0[CCOP_SCU::BADVADDR] == 0x4F801D80)
	{
		context->m_State.nCOP0[CCOP_SCU::EPC];
		int i = 0;
		i++;
	}
}

void CMIPSInstructionFactory::CheckTLBValidity(bool isWrite)
{
	uint8 nRS = (uint8)((m_nOpcode >> 21) & 0x001F);
	uint16 nImmediate = (uint16)((m_nOpcode >> 0) & 0xFFFF);

	if(m_pCtx->m_pAddrTranslator == &CMIPS::TranslateAddress64) return;

	//Push context
	m_codeGen->PushCtx();

	//Push low part of address
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[nRS].nV[0]));
	if(nImmediate != 0)
	{
		m_codeGen->PushCst((int16)nImmediate);
		m_codeGen->Add();
	}

	//Call
	m_codeGen->Call(reinterpret_cast<void*>(m_pCtx->m_pAddrTranslator), 2, true);

	m_codeGen->PushCst(MIPS_INVALID_PC);
	m_codeGen->BeginIf(Jitter::CONDITION_EQ);
	{
		m_codeGen->PushCst(m_nAddress);
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nCOP0[CCOP_SCU::EPC]));

		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[nRS].nV[0]));
		if(nImmediate != 0)
		{
			m_codeGen->PushCst((int16)nImmediate);
			m_codeGen->Add();
		}
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nCOP0[CCOP_SCU::BADVADDR]));

		m_codeGen->PushCst(isWrite ? MIPS_EXCEPTION_TLB_WRITE : MIPS_EXCEPTION_TLB_READ);
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nHasException));

		m_codeGen->JumpTo(reinterpret_cast<void*>(&HandleTLBError));
	}
	m_codeGen->EndIf();
}

void CMIPSInstructionFactory::ComputeMemAccessAddr()
{
	uint8 nRS = (uint8)((m_nOpcode >> 21) & 0x001F);
	uint16 nImmediate = (uint16)((m_nOpcode >> 0) & 0xFFFF);

	if(m_pCtx->m_pAddrTranslator == &CMIPS::TranslateAddress64)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[nRS].nV[0]));
		if(nImmediate != 0)
		{
			m_codeGen->PushCst((int16)nImmediate);
			m_codeGen->Add();
		}
		m_codeGen->PushCst(0x1FFFFFFF);
		m_codeGen->And();
	}
	else
	{
		//TODO: Compute the complete 64-bit address

		//Translate the address

		//Push context
		m_codeGen->PushCtx();

		//Push low part of address
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[nRS].nV[0]));
		if(nImmediate != 0)
		{
			m_codeGen->PushCst((int16)nImmediate);
			m_codeGen->Add();
		}

		//Call
		m_codeGen->Call(reinterpret_cast<void*>(m_pCtx->m_pAddrTranslator), 2, true);
	}
}

void CMIPSInstructionFactory::Branch(Jitter::CONDITION condition)
{
	uint16 nImmediate = (uint16)(m_nOpcode & 0xFFFF);

	m_codeGen->PushCst(MIPS_INVALID_PC);
	m_codeGen->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));

	m_codeGen->BeginIf(condition);
	{
		m_codeGen->PushCst((m_nAddress + 4) + CMIPS::GetBranch(nImmediate));
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));
	}
	m_codeGen->EndIf();
}

void CMIPSInstructionFactory::BranchLikely(Jitter::CONDITION condition)
{
	uint16 nImmediate = (uint16)(m_nOpcode & 0xFFFF);

	m_codeGen->PushCst(MIPS_INVALID_PC);
	m_codeGen->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));

	m_codeGen->BeginIf(condition);
	{
		m_codeGen->PushCst((m_nAddress + 4) + CMIPS::GetBranch(nImmediate));
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));
	}
	m_codeGen->Else();
	{
		m_codeGen->PushCst(m_nAddress + 8);
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nPC));
		m_codeGen->Goto(m_codeGen->GetFinalBlockLabel());
	}
	m_codeGen->EndIf();
}

void CMIPSInstructionFactory::Illegal()
{
#ifdef _DEBUG
	m_codeGen->Break();
#endif
}
