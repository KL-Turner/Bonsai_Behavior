using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Linq;
using System.Reactive.Linq;
using Bonsai;

[Combinator]
[Description("Polls a digital channel at regular intervals using a Timer node.")]
[WorkflowElementCategory(ElementCategory.Transform)]
public class SampleDigitalChannel
{
    // Main Process method
    public IObservable<int> Process(IObservable<long> Timer, IObservable<bool> DigitalInput)
    {
        // Combine the Timer with the DigitalInput, starting with an initial false value to trigger CombineLatest
        return Timer
            .CombineLatest(DigitalInput.StartWith(false).DistinctUntilChanged(), 
                           (time, inputState) => inputState ? 1 : 0);
    }
}